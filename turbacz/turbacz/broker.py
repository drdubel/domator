import asyncio
import json
import logging
from datetime import UTC, datetime
from time import time

import namer

import turbacz.metrics as metrics
from turbacz.connection_manager import connection_manager
from turbacz.database import db_call
from turbacz.ha.bridge import ha_bridge
from turbacz.mesh_metrics import record_node_metrics
from turbacz.mqtt_client import mqtt
from turbacz.mqtt_validation import normalize_payload, validate_payload
from turbacz.presence import PLAN_REFRESH, command_payload, plan_id, plan_payload, report_config
from turbacz.security import RateLimiter
from turbacz.settings import config
from turbacz.state_manager import state_manager
from turbacz.websocket import ws_manager
from turbacz.zigbee import handle_zigbee_message
from turbacz.zigbee_knob import mesh_switch_config

logger = logging.getLogger(__name__)
_device_task: asyncio.Task | None = None
_ha_task: asyncio.Task | None = None
_message_limiter = RateLimiter()
_config_requests = RateLimiter()  # settings reads asked of presence sensors
_plan_sends = RateLimiter()  # schedule plans sent to presence sensors


async def stop_background_tasks():
    global _device_task, _ha_task
    tasks = [task for task in (_device_task, _ha_task, ha_bridge._resync_task) if task is not None]
    for task in tasks:
        task.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
    _device_task = _ha_task = ha_bridge._resync_task = None


async def periodic_check_devices(interval: int = 15):
    """Periodically check if relays are online."""
    while True:
        try:
            await state_manager.check_relays_if_online()
            await state_manager.check_switches_if_online()

        except Exception as e:
            print(f"Error checking relays/switches: {e}")

        for relay_id in ha_bridge.relay_ids:
            ha_bridge.publish_relay_availability(relay_id, state_manager.is_relay_online(relay_id))

        await state_manager.send_online_status()
        await asyncio.sleep(interval)


@mqtt.on_connect()
def connect(client, flags, rc, properties):
    global _device_task, _ha_task
    mqtt.client.subscribe("/blind/pos")
    mqtt.client.subscribe("/heating/metrics")
    mqtt.client.subscribe("/relay/state/+")
    mqtt.client.subscribe("/switch/state/+")
    if config.zigbee.enabled:
        mqtt.client.subscribe(f"{config.zigbee.base_topic}/#")

    if config.ha.enabled:
        ha_bridge.reset_published_state()
        for topic in ha_bridge.command_subscriptions():
            mqtt.client.subscribe(topic)
        if _ha_task is None or _ha_task.done():
            _ha_task = asyncio.create_task(ha_bridge.resync_loop())
        else:
            ha_bridge.schedule_resync()

    if _device_task is None or _device_task.done():
        _device_task = asyncio.create_task(periodic_check_devices())

    logger.info("Connected: %s %s %s %s", client, flags, rc, properties)


@mqtt.on_message()
async def message(client, topic, payload, qos, properties):
    # Topic throttling bounds backend work. Publisher identity is only known
    # to the broker, which must enforce its own per-client quotas.
    if len(payload) > config.security.mqtt_max_bytes:
        return
    if not _message_limiter.allow(topic, config.security.mqtt_messages_per_minute):
        return
    try:
        payload_str = normalize_payload(topic, payload.decode("utf-8"))
        validate_payload(topic, payload_str)
        if topic.startswith(f"{config.zigbee.base_topic}/"):
            if config.zigbee.enabled:
                await handle_zigbee_message(topic, payload_str, retained=bool(properties and properties.get("retain")))
        else:
            await dispatch_message(topic, payload_str)
    except (UnicodeError, ValueError, TypeError, KeyError, OverflowError, RecursionError):
        logger.warning("Rejected malformed MQTT message")
    except Exception:
        logger.exception("MQTT message processing failed")


async def dispatch_message(topic, payload_str):
    """
    Handle incoming MQTT messages based on topic.
    """

    if topic.startswith(f"{config.zigbee.base_topic}/"):
        if config.zigbee.enabled:
            await handle_zigbee_message(topic, payload_str)
        return

    if config.ha.enabled and topic.startswith(f"{config.ha.base_topic}/"):
        await ha_bridge.handle_command(topic, payload_str)
        return

    if topic == "/heating/metrics":
        await handle_heating_metrics(payload_str)

    elif topic == "/blind/pos":
        await handle_blind_position(payload_str)

    elif topic.startswith("/relay/state/"):
        await handle_relay_state(payload_str, topic)

    elif topic == "/switch/state/root":
        await handle_root_state(payload_str)

    elif topic.startswith("/switch/state/"):
        await handle_switch_state(payload_str, topic)


async def handle_heating_metrics(payload_str):
    """
    Process heating metrics payload and update metrics and chart data.
    """
    try:
        data = json.loads(payload_str)

        # Update Prometheus metrics
        for probe in ("cold", "mixed", "hot"):
            metrics.water_temp.set({"probe": probe}, data[probe])

        metrics.pid_integral.set({}, data["integral"])
        metrics.pid_output.set({}, data["pid_output"])
        metrics.pid_target.set({}, data["target"])

        for multiplier in ("kp", "ki", "kd"):
            metrics.pid_multiplier.set({"multiplier": multiplier}, data[multiplier])

        await ws_manager.broadcast({**data, "timestamp": time()}, "/heating/ws/")
        await ha_bridge.on_heating_metrics(data)

    except (json.JSONDecodeError, KeyError) as e:
        logger.error("Error processing heating metrics: %s", e)


async def handle_blind_position(payload_str):
    """
    Process blind position payload.
    """
    parts = payload_str.split()
    if len(parts) >= 2:
        data = {"blind": parts[0], "current_position": parts[1]}
        await ws_manager.broadcast(data, "/blinds/ws/")
    else:
        logger.warning("Invalid blind position payload: %s", payload_str)


async def handle_relay_state(payload_str, topic):
    """
    Process switch state payload from /relay/state/+ topic.
    """
    try:
        relay_id = int(topic.split("/")[-1])

        state = int(payload_str[1])
        output_id = chr(ord(payload_str[0]) - ord("A") + 97)

        await state_manager.update_state(relay_id, output_id, state)

    except ValueError as e:
        logger.error("Error processing relay state: %s", e)


async def handle_switch_state(payload_str, topic):
    """
    Process switch state payload from /switch/state/+ topic.

    Payload formats (from firmware):
      - Numeric payload: ping time in ms
      - Single char, e.g. 'a': toggle button press event
      - Two chars, e.g. 'a1' (pressed) or 'a0' (released): stateful button event

    Long-press detection for blind pairs is handled by the ESP32 root node firmware.
    """
    try:
        switch_id = int(topic.split("/")[-1])

        if not payload_str or not payload_str[0].isalpha():
            # Numeric payload = ping time
            ping_time = int(payload_str)
            logger.debug("Switch ID: %s, Ping Time: %d ms", switch_id, ping_time)
            state_manager.update_device_ping(switch_id, ping_time)
            return

        button_id = payload_str[0].lower()

        logger.debug("Switch ID: %s, Button ID: %s", switch_id, button_id)

        await ws_manager.broadcast(
            {"type": "switch_state", "switch_id": switch_id, "button_id": button_id},
            "/rcm/ws/",
        )

        # A presence change is reported on every sensor input; 'a' carries it.
        if (
            button_id == "a"
            and len(payload_str) > 1
            and state_manager.get_device_role(switch_id).get("type") == "presence"
        ):
            state_manager.update_presence(switch_id, presence=int(payload_str[1]))
            await ws_manager.broadcast(
                {"type": "presence_state", "switch_id": switch_id, **state_manager.get_presence(switch_id)},
                "/rcm/ws/",
            )

        # Stateful (type 1) buttons report both edges as 'a1'/'a0'; toggle
        # buttons send a bare 'a', which is always a press.
        event_type = "release" if len(payload_str) > 1 and payload_str[1] == "0" else "press"
        await ha_bridge.on_button_event(switch_id, button_id, event_type)

    except (ValueError, IndexError) as e:
        logger.error("Error processing switch state: %s", e)


async def ensure_presence_sensor(device_id: int, switches: dict) -> tuple[str, dict | None]:
    """Register a reporting presence sensor, or convert its old switch record.

    Returns its name and the settings it last reported, if any.
    """
    name = switches[device_id][0] if device_id in switches else namer.generate(category="astronomy")
    sensors = await db_call(connection_manager.get_presence_sensors)
    if device_id not in sensors:
        await db_call(connection_manager.add_presence_sensor, device_id, name)
        # The root routes the inputs by type, so it needs them before the first event.
        buttons = await db_call(connection_manager.get_all_buttons)
        mqtt.client.publish("/switch/cmd/root", json.dumps({"type": "button_types", "data": mesh_switch_config(buttons)}))
        await ws_manager.broadcast({"type": "update"}, "/rcm/ws/")
    return name, sensors.get(device_id)


async def sync_presence_plan(data):
    """Send a sensor the plan of its schedule when it follows another or runs low.

    The plan is lost when the sensor restarts and lasts PLAN_DAYS, so this
    also restores it after a restart and renews it about once a day.
    """
    if "plan" not in data:
        return  # firmware without schedules
    device_id = data["deviceId"]
    schedule = (await db_call(connection_manager.get_presence_schedules)).get(device_id)
    expected = plan_id(schedule)
    stale = data["plan"] != expected or (expected and data["planLeft"] < PLAN_REFRESH)
    if stale and _plan_sends.allow(device_id, 1, 60):
        mqtt.client.publish(f"/switch/cmd/{device_id}", plan_payload(schedule, datetime.now(UTC)))


async def handle_presence_report(data):
    """Live radar data, or the configuration a sensor read back from its radar."""
    device_id = data["deviceId"]
    if data["report"] == "live":
        state_manager.update_presence(device_id, presence=data["presence"], distance=data["distance"], radar=1)
        await ws_manager.broadcast(
            {
                "type": "presence_live",
                "switch_id": device_id,
                "presence": data["presence"],
                "distance": data["distance"],
                "energy": data["energy"],
                "calibrating": data.get("calibrating", 0),
            },
            "/rcm/ws/",
        )
        return

    await ensure_presence_sensor(device_id, await db_call(connection_manager.get_switches))
    sensor_config = report_config(data)
    await db_call(connection_manager.set_presence_config, device_id, sensor_config)
    await ws_manager.broadcast(
        {"type": "presence_config", "switch_id": device_id, "config": sensor_config}, "/rcm/ws/"
    )


async def handle_root_state(payload_str):
    """
    Process root switch state payload.
    """
    try:
        data = json.loads(payload_str)
        logger.debug("Root State Data: %s", data)  # Debug log

    except json.JSONDecodeError:
        logger.error("Error processing root state JSON payload: %s", payload_str)
        return

    status = data.get("status", "")

    if status == "connected":
        connections = await db_call(connection_manager.get_all_connections)
        logger.debug("Connections: %s", connections)  # Debug log

        mqtt.client.publish("/switch/cmd/root", json.dumps({"type": "connections", "data": mesh_switch_config(connections)}))
        blind_pairs = await db_call(connection_manager.get_blind_pairs)
        mqtt.client.publish("/switch/cmd/root", json.dumps({"type": "blind_pairs", "data": blind_pairs}))
        mqtt.client.publish(
            "/switch/cmd/root", json.dumps({"type": "button_types", "data": mesh_switch_config(await db_call(connection_manager.get_all_buttons))})
        )

        # Sync per-output auto-off timers to relay boards.
        outputs = await db_call(connection_manager.get_outputs)
        auto_off_payload: dict[str, dict[str, int]] = {}
        for relay_id, relay_outputs in outputs.items():
            relay_key = str(relay_id)
            auto_off_payload[relay_key] = {}
            for output_id, output_meta in relay_outputs.items():
                timeout_seconds = 0
                if isinstance(output_meta, (tuple, list)) and len(output_meta) > 3:
                    timeout_seconds = max(int(output_meta[3] or 0), 0)
                auto_off_payload[relay_key][str(output_id)] = timeout_seconds

        mqtt.client.publish(
            "/switch/cmd/root",
            json.dumps({"type": "auto_off", "data": auto_off_payload}),
        )
        return

    if status == "disconnected":
        return

    if "report" in data:
        await handle_presence_report(data)
        return

    relays = await db_call(connection_manager.get_relays)
    switches = await db_call(connection_manager.get_switches)
    if data["type"] in {"switch", "remote"}:
        if data["deviceId"] in switches:
            data["name"] = switches[data["deviceId"]][0]
        else:
            name = namer.generate(category="astronomy")
            data["name"] = name
            buttons = 3
            if data["type"] == "remote":
                buttons = data.get("buttons", 7)  # Older C3 remotes report no count.
                if type(buttons) is not int or not 1 <= buttons <= 7:
                    buttons = 7
            await db_call(connection_manager.add_switch, data["deviceId"], name, buttons)
            await ws_manager.broadcast({"type": "update"}, "/rcm/ws/")

    elif data["type"] == "presence":
        data["name"], sensor_config = await ensure_presence_sensor(data["deviceId"], switches)
        presence = {key: data.get(key, 0) for key in ("presence", "distance", "radar")}
        if "active" in data:  # whether presence switches outputs, or the schedule pauses it
            presence["active"] = data["active"]
        state_manager.update_presence(data["deviceId"], **presence)
        # A sensor reports its settings once its radar is set up; that report is
        # lost when the root or Turbacz is not connected yet, so ask again.
        if sensor_config is None and data.get("radar") and _config_requests.allow(data["deviceId"], 1, 300):
            mqtt.client.publish(f"/switch/cmd/{data['deviceId']}", command_payload({"action": "read"}))
        await sync_presence_plan(data)

    elif data["type"] == "relay8" or data["type"] == "relay16":
        if data["deviceId"] in relays:
            data["name"] = relays[data["deviceId"]][0]  # Extract name from tuple
        else:
            name = namer.generate(category="animals")
            data["name"] = name

            if data["type"] == "relay8":
                await db_call(connection_manager.add_relay, data["deviceId"], name, 8)
            else:
                await db_call(connection_manager.add_relay, data["deviceId"], name, 16)
            await db_call(connection_manager.add_switch, data["deviceId"], name, 8)

            await ws_manager.broadcast({"type": "update"}, "/rcm/ws/")

    else:
        logger.warning(f"Unknown device type for ID {data['deviceId']}: {data['type']}")
        return

    if data.get("isRoot") == 1:
        connection_manager.rootId = data["deviceId"]

    state_manager.set_device_role(data["deviceId"], data)
    state_manager.mark_relay_online(data["deviceId"], int(time()))
    state_manager.mark_switch_online(data["deviceId"], int(time()))
    ha_bridge.publish_relay_availability(data["deviceId"], True)
    # Presence sensors run the C3 switch image, so they share its newest version.
    image = "switch" if data["type"] == "presence" else data["type"]
    state_manager.set_firmware_version(data["deviceId"], image, data["firmware"])
    state_manager.set_device_rssi(data["deviceId"], int(data["rssi"]))

    if data["parentId"] in relays:
        parent_name = relays[data["parentId"]][0]  # Extract name from tuple

    elif data["parentId"] in switches:
        parent_name = switches[data["parentId"]][0]

    elif data["parentId"] == data["deviceId"] or data["parentId"] == connection_manager.rootId:
        parent_name = "root"

    else:
        parent_name = "unknown"

    if data["type"] != "remote":
        mqtt.client.publish("/switch/cmd/" + str(data["deviceId"]), "P")

    try:
        record_node_metrics(data, parent_name, state_manager.get_device_ping(data["deviceId"]))
    except (ValueError, TypeError, KeyError, OverflowError):
        logger.warning("Rejected invalid node metrics")
