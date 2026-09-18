"""Derive the desired Home Assistant entity set from the Turbacz registry.

This is the only part of the bridge that reads the database. It turns relays,
outputs, sections, blind pairs, switches and buttons into discovery payloads
plus the lookup tables the bridge needs to route state and commands.
"""

import logging
from dataclasses import dataclass, field

from turbacz.ha import discovery
from turbacz.ha import topics as T

logger = logging.getLogger(__name__)

UNCATEGORIZED_SECTION_ID = 0
UNCATEGORIZED_SECTION_NAME = "Uncategorized"

DEFAULT_OUTPUT_PREFIX = "Output "

# key -> (label, unit, device_class, diagnostic)
HEATING_SENSORS: dict[str, tuple[str, str | None, str | None, bool]] = {
    "cold": ("Cold probe", "°C", "temperature", False),
    "hot": ("Hot probe", "°C", "temperature", False),
    "pid_output": ("PID output", None, None, True),
    "integral": ("PID integral", None, None, True),
    "kp": ("PID Kp", None, None, True),
    "ki": ("PID Ki", None, None, True),
    "kd": ("PID Kd", None, None, True),
}


@dataclass(frozen=True)
class DesiredEntity:
    component: str
    key: str
    uid: str
    discovery_topic: str
    payload: dict


@dataclass(frozen=True)
class CoverSpec:
    relay_id: int
    power_id: str
    direction_id: str

    @property
    def key(self) -> str:
        return T.output_key(self.relay_id, self.power_id)


@dataclass
class Registry:
    """A snapshot of what Home Assistant should currently see."""

    entities: dict[str, DesiredEntity] = field(default_factory=dict)
    #: (relay_id, output_id) -> light key
    lights: dict[tuple[int, str], str] = field(default_factory=dict)
    #: cover key -> spec
    covers: dict[str, CoverSpec] = field(default_factory=dict)
    #: (relay_id, output_id) -> cover key, for both legs of every pair
    output_covers: dict[tuple[int, str], str] = field(default_factory=dict)
    #: (switch_id, button_id) -> event types
    buttons: dict[tuple[int, str], list[str]] = field(default_factory=dict)
    #: every relay that owns at least one entity, for availability publishing
    relay_ids: set[int] = field(default_factory=set)

    def add(self, entity: DesiredEntity) -> None:
        self.entities[entity.discovery_topic] = entity


def _blind_pair_name(pair: dict) -> str:
    """Best available human name for a blind pair.

    ``get_relay_blind_pairs_with_names`` falls back to the relay name when the
    pair has no custom name, which would give every blind on a board the same
    label. Prefer, in order: the custom pair name, the power output's name,
    then a relay-qualified fallback.
    """
    custom = pair.get("name") or ""
    relay_name = pair.get("relay_name") or ""
    power_name = pair.get("power_name") or ""

    if custom and custom != relay_name:
        return custom
    if power_name and not power_name.startswith(DEFAULT_OUTPUT_PREFIX):
        return power_name
    return f"{relay_name} {pair['power_id']}".strip()


def build_registry(cm, base: str, prefix: str) -> Registry:
    """Build the desired entity set. ``cm`` is a ConnectionManager."""
    registry = Registry()

    sections: dict[int, str] = cm.get_sections() or {}
    outputs = cm.get_outputs()
    named_outputs = cm.get_named_outputs()
    blind_outputs = cm.get_blind_pair_outputs()

    def section_device_for(section_id: int) -> dict:
        if section_id in sections:
            return discovery.section_device(section_id, sections[section_id])
        return discovery.section_device(UNCATEGORIZED_SECTION_ID, UNCATEGORIZED_SECTION_NAME)

    # -- lights: every named output that is not a leg of a blind pair --------
    for relay_id, relay_outputs in named_outputs.items():
        for output_id, (name, section_id, _idx, _auto_off) in relay_outputs.items():
            if (int(relay_id), output_id) in blind_outputs:
                continue

            key = T.output_key(relay_id, output_id)
            payload = discovery.build_light(base, relay_id, output_id, name, section_device_for(section_id))
            registry.add(
                DesiredEntity(
                    component=T.LIGHT,
                    key=key,
                    uid=payload["uniq_id"],
                    discovery_topic=T.discovery_topic(prefix, T.LIGHT, key),
                    payload=payload,
                )
            )
            registry.lights[(int(relay_id), output_id)] = key
            registry.relay_ids.add(int(relay_id))

    # -- covers: one per blind pair ------------------------------------------
    for pair in cm.get_relay_blind_pairs_with_names():
        relay_id = int(pair["relay_id"])
        power_id = pair["power_id"]
        direction_id = pair["direction_id"]

        spec = CoverSpec(relay_id=relay_id, power_id=power_id, direction_id=direction_id)
        # A cover belongs to whichever section its power output sits in.
        power_meta = outputs.get(relay_id, {}).get(power_id)
        section_id = power_meta[1] if power_meta else UNCATEGORIZED_SECTION_ID

        payload = discovery.build_cover(
            base, relay_id, power_id, _blind_pair_name(pair), section_device_for(section_id)
        )
        registry.add(
            DesiredEntity(
                component=T.COVER,
                key=spec.key,
                uid=payload["uniq_id"],
                discovery_topic=T.discovery_topic(prefix, T.COVER, spec.key),
                payload=payload,
            )
        )
        registry.covers[spec.key] = spec
        registry.output_covers[(relay_id, power_id)] = spec.key
        registry.output_covers[(relay_id, direction_id)] = spec.key
        registry.relay_ids.add(relay_id)

    # -- button events: grouped by physical wall panel -----------------------
    switches = cm.get_switches()
    for switch_id, buttons in cm.get_all_buttons().items():
        switch_id = int(switch_id)
        switch_name = switches.get(switch_id, (f"Switch {switch_id}",))[0]
        device = discovery.switch_device(switch_id, switch_name)

        for button_id, button_type in buttons.items():
            # Type 1 buttons are stateful and report both edges.
            event_types = ["press", "release"] if int(button_type) == 1 else ["press"]
            key = T.button_key(switch_id, button_id)
            payload = discovery.build_event(
                base, switch_id, button_id, f"Button {button_id.upper()}", event_types, device
            )
            registry.add(
                DesiredEntity(
                    component=T.EVENT,
                    key=key,
                    uid=payload["uniq_id"],
                    discovery_topic=T.discovery_topic(prefix, T.EVENT, key),
                    payload=payload,
                )
            )
            registry.buttons[(switch_id, button_id)] = event_types

    # -- heating --------------------------------------------------------------
    water_heater = discovery.build_water_heater(base)
    registry.add(
        DesiredEntity(
            component=T.WATER_HEATER,
            key=T.HEATING_KEY,
            uid=water_heater["uniq_id"],
            discovery_topic=T.discovery_topic(prefix, T.WATER_HEATER, T.HEATING_KEY),
            payload=water_heater,
        )
    )

    for probe, (label, unit, device_class, diagnostic) in HEATING_SENSORS.items():
        key = T.heating_sensor_key(probe)
        payload = discovery.build_sensor(
            base, key, label, unit=unit, device_class=device_class, diagnostic=diagnostic
        )
        registry.add(
            DesiredEntity(
                component=T.SENSOR,
                key=key,
                uid=payload["uniq_id"],
                discovery_topic=T.discovery_topic(prefix, T.SENSOR, key),
                payload=payload,
            )
        )

    return registry
