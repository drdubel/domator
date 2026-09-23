"""Home Assistant MQTT Discovery bridge.

Publishes retained discovery configs derived from the Turbacz registry, mirrors
device state onto ``domator/...`` topics, and translates Home Assistant
commands back into the ``/relay/cmd/...`` and ``/heating/cmd`` protocol the
mesh already speaks.

Home Assistant is a *user* surface here: lights, blinds, the heating loop and
wall buttons. Administration -- firmware updates, mesh topology, wiring
configuration -- deliberately stays in the Turbacz web UI.
"""

import asyncio
import json
import logging

from turbacz.ha import topics as T
from turbacz.ha.registry import HEATING_SENSORS, Registry, build_registry
from turbacz.settings import config

logger = logging.getLogger(__name__)

RESYNC_DEBOUNCE = 1.0

_OPEN, _CLOSE, _STOP = "OPEN", "CLOSE", "STOP"


class HABridge:
    def __init__(self):
        self._registry = Registry()
        # topic -> payload last published, so a resync that changes nothing
        # publishes nothing.
        self._published: dict[str, str] = {}
        # cover key -> {"power": 0|1, "direction": 0|1, "last_direction": 0|1}
        self._cover_state: dict[str, dict[str, int]] = {}
        self._resync_task: asyncio.Task | None = None
        self._lock = asyncio.Lock()
        # Both are resolved lazily and can be replaced in tests, so the bridge
        # can be exercised without a database or a broker. connection_manager
        # opens a PostgreSQL connection at import time and imports this module
        # in turn, so it must not be a module-level import here.
        self._cm = None
        self._client = None

    def cm(self):
        if self._cm is None:
            from turbacz.connection_manager import connection_manager

            self._cm = connection_manager
        return self._cm

    def client(self):
        if self._client is None:
            from turbacz.mqtt_client import mqtt

            self._client = mqtt.client
        return self._client

    # -- helpers --------------------------------------------------------------

    @property
    def enabled(self) -> bool:
        return config.ha.enabled

    @property
    def _base(self) -> str:
        return config.ha.base_topic

    @property
    def relay_ids(self) -> set[int]:
        """Relays that own at least one HA entity, for availability publishing."""
        return self._registry.relay_ids

    def _publish(self, topic: str, payload: str, *, retain: bool = True, qos: int = 1) -> None:
        try:
            self.client().publish(topic, payload, qos=qos, retain=retain)
        except Exception as exc:
            logger.error("HA bridge failed to publish %s: %s", topic, exc)

    def _publish_state(self, topic: str, payload: str) -> None:
        """Retained state publish that skips no-op republishes."""
        if self._published.get(topic) == payload:
            return
        self._published[topic] = payload
        self._publish(topic, payload)

    # -- discovery ------------------------------------------------------------

    async def apply(self) -> None:
        """Publish discovery for everything that should exist, clear what should not.

        Idempotent: safe to call on every connect and on a timer.
        """
        if not self.enabled:
            return

        async with self._lock:
            try:
                registry = build_registry(self.cm(), self._base, config.ha.discovery_prefix)
            except Exception as exc:
                logger.error("HA bridge could not build registry: %s", exc, exc_info=True)
                return

            self._registry = registry

            self._publish(T.bridge_status_topic(self._base), "online")
            self._publish(T.heating_mode_state_topic(self._base), "performance")

            try:
                applied = self.cm().get_applied_topics()
            except Exception as exc:
                logger.error("HA bridge could not read applied topics: %s", exc)
                return

            for topic, entity in registry.entities.items():
                payload = json.dumps(entity.payload, ensure_ascii=False)
                if self._published.get(topic) == payload and topic in applied:
                    continue

                self._publish(topic, payload)
                self._published[topic] = payload
                try:
                    self.cm().upsert_applied_topic(topic, entity.uid)
                except Exception as exc:
                    logger.error("HA bridge could not record applied topic %s: %s", topic, exc)

            # An empty retained payload tells HA to delete the entity.
            for topic in set(applied) - set(registry.entities):
                self._publish(topic, "")
                self._published.pop(topic, None)
                try:
                    self.cm().delete_applied_topic(topic)
                except Exception as exc:
                    logger.error("HA bridge could not clear applied topic %s: %s", topic, exc)
                logger.info("HA bridge cleared discovery topic %s", topic)

    def schedule_resync(self) -> None:
        """Debounced re-apply, for registry edits that should show up promptly."""
        if not self.enabled:
            return

        async def _later():
            await asyncio.sleep(RESYNC_DEBOUNCE)
            await self.apply()

        if self._resync_task and not self._resync_task.done():
            self._resync_task.cancel()

        try:
            self._resync_task = asyncio.create_task(_later())
        except RuntimeError:
            # No running loop (e.g. a registry edit during import); the periodic
            # resync will pick the change up.
            logger.debug("HA bridge resync requested with no running loop")

    async def resync_loop(self) -> None:
        """Self-healing periodic re-apply.

        The registry is mutated from a dozen REST endpoints and several
        WebSocket branches. Rather than hooking every one, re-derive on a timer:
        `apply` is a diff, so a run with no changes publishes nothing.
        """
        if not self.enabled:
            return

        await self.apply()
        while True:
            await asyncio.sleep(max(config.ha.resync_interval, 5))
            try:
                await self.apply()
            except Exception as exc:
                logger.error("HA bridge resync failed: %s", exc, exc_info=True)

    # -- outbound state -------------------------------------------------------

    def publish_relay_availability(self, relay_id: int, online: bool) -> None:
        if not self.enabled:
            return
        self._publish_state(
            T.relay_availability_topic(self._base, relay_id), "online" if online else "offline"
        )

    async def on_relay_state(self, relay_id: int, output_id: str, state: int) -> None:
        """A relay output changed: update the light, or the cover it belongs to."""
        if not self.enabled:
            return

        relay_id = int(relay_id)

        light_key = self._registry.lights.get((relay_id, output_id))
        if light_key is not None:
            self._publish_state(
                T.state_topic(self._base, T.LIGHT, light_key), "ON" if state else "OFF"
            )
            return

        cover_key = self._registry.output_covers.get((relay_id, output_id))
        if cover_key is not None:
            self._update_cover(cover_key, output_id, int(state))

    def _update_cover(self, cover_key: str, output_id: str, state: int) -> None:
        spec = self._registry.covers.get(cover_key)
        if spec is None:
            return

        tracked = self._cover_state.setdefault(cover_key, {})
        if output_id == spec.power_id:
            tracked["power"] = state
        elif output_id == spec.direction_id:
            tracked["direction"] = state

        # Remember which way it was last driven, so that once it stops we can
        # say whether it ended up open or closed.
        if tracked.get("power") == 1 and "direction" in tracked:
            tracked["last_direction"] = tracked["direction"]

        value = self._cover_value(tracked)
        if value is not None:
            self._publish_state(T.state_topic(self._base, T.COVER, cover_key), value)

    @staticmethod
    def _cover_value(tracked: dict[str, int]) -> str | None:
        """Blinds have no encoder, so position is inferred, never measured."""
        power = tracked.get("power")
        if power is None:
            return None

        if power == 1:
            direction = tracked.get("direction")
            if direction is None:
                return None
            return "opening" if direction == 0 else "closing"

        last = tracked.get("last_direction")
        if last is None:
            # Stopped, and we have never seen it move: we genuinely do not know.
            return None
        return "open" if last == 0 else "closed"

    async def on_heating_metrics(self, data: dict) -> None:
        if not self.enabled:
            return

        base = self._base
        if "mixed" in data:
            self._publish_state(T.heating_current_topic(base), str(data["mixed"]))
        if "target" in data:
            self._publish_state(T.heating_target_state_topic(base), str(data["target"]))

        for probe in HEATING_SENSORS:
            if probe in data:
                self._publish_state(
                    T.state_topic(base, T.SENSOR, T.heating_sensor_key(probe)), str(data[probe])
                )

    async def on_button_event(self, switch_id: int, button_id: str, event_type: str) -> None:
        if not self.enabled:
            return

        event_types = self._registry.buttons.get((int(switch_id), button_id))
        if event_types is None or event_type not in event_types:
            return

        # Never retained: a retained event would replay on every HA restart.
        self._publish(
            T.state_topic(self._base, T.EVENT, T.button_key(switch_id, button_id)),
            json.dumps({"event_type": event_type}),
            retain=False,
            qos=0,
        )

    # -- inbound commands -----------------------------------------------------

    def command_subscriptions(self) -> list[str]:
        base = self._base
        return [
            f"{base}/{T.LIGHT}/+/set",
            f"{base}/{T.COVER}/+/set",
            T.heating_target_command_topic(base),
        ]

    async def handle_command(self, topic: str, payload: str) -> bool:
        """Route a ``domator/...`` command. Returns True if it was ours."""
        if not self.enabled or not topic.startswith(f"{self._base}/"):
            return False

        if topic == T.heating_target_command_topic(self._base):
            self._handle_heating_command(payload)
            return True

        parts = topic.split("/")
        if len(parts) != 4 or parts[3] != "set":
            return False

        _, component, key, _ = parts
        if component == T.LIGHT:
            self._handle_light_command(key, payload)
            return True
        if component == T.COVER:
            self._handle_cover_command(key, payload)
            return True

        return False

    def _handle_light_command(self, key: str, payload: str) -> None:
        if key not in {v for v in self._registry.lights.values()}:
            logger.warning("HA bridge got a command for unknown light %s", key)
            return

        relay_id, _, output_id = key.partition("_")
        state = 1 if payload.strip().upper() == "ON" else 0
        self.client().publish(f"/relay/cmd/{relay_id}", f"{output_id}{state}")

    def _handle_cover_command(self, key: str, payload: str) -> None:
        spec = self._registry.covers.get(key)
        if spec is None:
            logger.warning("HA bridge got a command for unknown cover %s", key)
            return

        action = {_OPEN: "up", _CLOSE: "down", _STOP: "stop"}.get(payload.strip().upper())
        if action is None:
            logger.warning("HA bridge got an unknown cover command %r for %s", payload, key)
            return

        from turbacz.mqtt_client import publish_blind_action

        publish_blind_action(self.client(), spec.relay_id, spec.power_id, spec.direction_id, action)

    def _handle_heating_command(self, payload: str) -> None:
        try:
            target = float(payload)
        except (TypeError, ValueError):
            logger.warning("HA bridge got an invalid heating target %r", payload)
            return

        self.client().publish("/heating/cmd", f"t{target}")


ha_bridge = HABridge()
