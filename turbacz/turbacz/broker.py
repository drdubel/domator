import asyncio
import json
import logging
from time import time

import httpx
import namer
from fastapi_mqtt import FastMQTT, MQTTConfig

import turbacz.metrics as metrics
from turbacz.connection_manager import connection_manager
from turbacz.settings import config
from turbacz.state_manager import state_manager
from turbacz.websocket import ws_manager

logger = logging.getLogger(__name__)


mqtt_config = MQTTConfig(
    host=config.mqtt.host,
    port=config.mqtt.port,
    keepalive=60,
    username=config.mqtt.username,
    password=config.mqtt.password,
)

mqtt = FastMQTT(config=mqtt_config)


async def periodic_check_devices(interval: int = 15):
    """Periodically check if relays are online."""
    while True:
        try:
            await state_manager.check_relays_if_online()
            await state_manager.check_switches_if_online()

        except Exception as e:
            print(f"Error checking relays/switches: {e}")

        await state_manager.send_online_status()
        await asyncio.sleep(interval)


@mqtt.on_connect()
def connect(client, flags, rc, properties):
    mqtt.client.subscribe("/blind/pos")
    mqtt.client.subscribe("/heating/metrics")
    mqtt.client.subscribe("/relay/state/+")
    mqtt.client.subscribe("/switch/state/+")

    if config.ha.enabled:
        try:
            from turbacz.ha.apply import apply, get_command_topics
            from turbacz.ha.db import ha_db

            for topic in get_command_topics():
                mqtt.client.subscribe(topic, qos=1)
                logger.info("Subscribed to HA command topic: %s", topic)

            # Publish discovery configs for all current named outputs
            apply(mqtt.client, ha_db)
            logger.info("Published HA discovery configs on connect")
        except Exception as exc:
            logger.error("Failed to initialise HA on connect: %s", exc)

    asyncio.create_task(periodic_check_devices())

    logger.info("Connected: %s %s %s %s", client, flags, rc, properties)


@mqtt.on_message()
async def message(client, topic, payload, qos, properties):
    """
    Handle incoming MQTT messages based on topic.
    """

    payload_str = payload.decode()

    if topic == "/heating/metrics":
        await handle_heating_metrics(payload_str)

    elif topic == "/blind/pos":
        await handle_blind_position(payload_str)

    elif topic.startswith("/relay/state/"):
        await handle_relay_state(payload_str, topic)

    elif topic.startswith("/switch/state/root"):
        await handle_root_state(payload_str)

    elif topic.startswith("/switch/state/"):
        await handle_switch_state(payload_str, topic)

    elif topic.startswith("domator/") and topic.endswith("/light/set"):
        await handle_ha_light_command(topic, payload_str)


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

        await ws_manager.broadcast(data, "/heating/ws/")

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

        if config.ha.enabled:
            _publish_ha_light_state(relay_id, output_id, state)

    except ValueError as e:
        logger.error("Error processing relay state: %s", e)


async def handle_switch_state(payload_str, topic):
    """
    Process switch state payload from /switch/state/+ topic.
    """
    try:
        switch_id = int(topic.split("/")[-1])

        if payload_str[0].isalpha():
            button_id = payload_str[0]

            logger.debug("Switch ID: %s, Button ID: %s", switch_id, button_id)  # Debug log

            await ws_manager.broadcast(
                {
                    "type": "switch_state",
                    "switch_id": switch_id,
                    "button_id": button_id,
                },
                "/rcm/ws/",
            )

        else:
            ping_time = int(payload_str)

            logger.debug("Switch ID: %s, Ping Time: %d ms", switch_id, ping_time)  # Debug log

            state_manager.update_device_ping(switch_id, ping_time)

    except ValueError as e:
        logger.error("Error processing switch state: %s", e)


async def handle_root_state(payload_str):
    """
    Process root switch state payload.
    """
    connections = connection_manager.get_all_connections()
    relays = connection_manager.get_relays()
    switches = connection_manager.get_switches()

    try:
        data = json.loads(payload_str)
        logger.debug("Root State Data: %s", data)  # Debug log

    except json.JSONDecodeError:
        logger.error("Error processing root state JSON payload: %s", payload_str)
        return

    status = data.get("status", "")

    if status == "connected":
        logger.debug("Connections: %s", connections)  # Debug log

        mqtt.client.publish("/switch/cmd/root", json.dumps({"type": "connections", "data": connections}))
        mqtt.client.publish(
            "/switch/cmd/root", json.dumps({"type": "button_types", "data": connection_manager.get_all_buttons()})
        )

        # Sync per-output auto-off timers to relay boards.
        outputs = connection_manager.get_outputs()
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

    url = f"{config.monitoring.metrics}/api/v2/write"
    if config.monitoring.labels:
        labels = "," + ",".join(f"{key}={value}" for key, value in config.monitoring.labels.items())
    else:
        labels = ""

    if data["type"] == "switch":
        if data["deviceId"] in switches:
            data["name"] = switches[data["deviceId"]][0]
        else:
            name = namer.generate(category="astronomy")
            data["name"] = name
            connection_manager.add_switch(data["deviceId"], name, 3)
            await ws_manager.broadcast({"type": "update"}, "/rcm/ws/")

    elif data["type"] == "relay8" or data["type"] == "relay16":
        if data["deviceId"] in relays:
            data["name"] = relays[data["deviceId"]][0]  # Extract name from tuple
        else:
            name = namer.generate(category="animals")
            data["name"] = name

            if data["type"] == "relay8":
                connection_manager.add_relay(data["deviceId"], name, 8)
            else:
                connection_manager.add_relay(data["deviceId"], name, 16)
            connection_manager.add_switch(data["deviceId"], name, 8)

            await ws_manager.broadcast({"type": "update"}, "/rcm/ws/")

    else:
        logger.warning(f"Unknown device type for ID {data['deviceId']}: {data['type']}")
        return

    if data.get("isRoot") == 1:
        connection_manager.rootId = data["deviceId"]

    state_manager.mark_relay_online(data["deviceId"], int(time()))
    state_manager.mark_switch_online(data["deviceId"], int(time()))
    state_manager.set_firmware_version(data["deviceId"], data["type"], data["firmware"])
    state_manager.set_device_rssi(data["deviceId"], int(data["rssi"]))

    # Re-publish HA discovery when a relay reconnects so HA stays in sync
    if config.ha.enabled and data["type"] in ("relay8", "relay16"):
        try:
            from turbacz.ha.apply import apply
            from turbacz.ha.db import ha_db

            apply(mqtt.client, ha_db)
        except Exception as exc:
            logger.warning("HA apply after relay connect failed: %s", exc)

    data["name"] = data["name"].replace(" ", "\\ ")

    if data["parentId"] in relays:
        parent_name = relays[data["parentId"]][0]  # Extract name from tuple

    elif data["parentId"] in switches:
        parent_name = switches[data["parentId"]][0]

    elif data["parentId"] == data["deviceId"] or data["parentId"] == connection_manager.rootId:
        parent_name = "root"

    else:
        parent_name = "unknown"

    data["parent_name"] = parent_name.replace(" ", "\\ ")

    mqtt.client.publish("/switch/cmd/" + str(data["deviceId"]), "P")

    if not config.monitoring.send_metrics:
        return

    metric_node = f"node_info,id={data['deviceId']},name={data['name']}{labels} uptime={data['uptime']},clicks={data['clicks']},free_heap={data['freeHeap']},ping_time={state_manager.get_device_ping(data['deviceId'])}"
    metric_mesh = f"mesh_node,id={data['deviceId']},name={data['name']},parent={data['parentId']},parent_name={data['parent_name']},firmware={data['firmware']},type={data['type']}{labels} rssi={data['rssi']}"

    logger.debug(metric_node)  # Debug log
    logger.debug(metric_mesh)  # Debug log

    async with httpx.AsyncClient() as client:
        response = await client.post(url, content=metric_node)
        if response.status_code != 204:
            logger.error("Failed to write metric for %s: %s", data["deviceId"], response.text)

        response = await client.post(url, content=metric_mesh)
        if response.status_code != 204:
            logger.error("Failed to write metric for %s: %s", data["deviceId"], response.text)


def _publish_ha_light_state(relay_id: int, output_id: str, state: int) -> None:
    """Publish the relay output state to the corresponding HA light state topic."""
    try:
        from turbacz.ha import topics as T

        outputs = connection_manager.get_outputs()
        output_info = outputs.get(relay_id, {}).get(output_id)
        if output_info is None:
            return
        _name, section_id, _idx, _auto = output_info
        area_id = f"s{section_id}"
        device_id = f"r{relay_id}_{output_id}"
        ha_topic = T.state_topic("home", area_id, device_id, "light")
        mqtt.client.publish(ha_topic, "ON" if state else "OFF", qos=1, retain=True)
    except Exception as exc:
        logger.error("Failed to publish HA light state for relay %s output %s: %s", relay_id, output_id, exc)


async def handle_ha_light_command(topic: str, payload_str: str) -> None:
    """Forward an ON/OFF command from Home Assistant to the physical relay.

    Expected topic: ``domator/home/s{section_id}/r{relay_id}_{output_id}/light/set``
    """
    try:
        parts = topic.split("/")
        # parts: ["domator", "home", "s<n>", "r<relay>_<output>", "light", "set"]
        device_part = parts[3]  # "r{relay_id}_{output_id}"
        if not device_part.startswith("r"):
            return
        relay_str, output_id = device_part[1:].rsplit("_", 1)
        relay_id = int(relay_str)
        state = 1 if payload_str.strip().upper() == "ON" else 0
        mqtt.client.publish(f"/relay/cmd/{relay_id}", f"{output_id.upper()}{state}")
    except (ValueError, IndexError) as exc:
        logger.error("HA light command error for topic %s: %s", topic, exc)
