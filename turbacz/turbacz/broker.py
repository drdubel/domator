import asyncio
import json
import logging
from time import time

import httpx
import namer
from fastapi_mqtt import FastMQTT, MQTTConfig

from turbacz import connection_manager, metrics
from turbacz.settings import config
from turbacz.state import state_manager
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

        await ws_manager.broadcast(
            {
                "type": "online_status",
                "online_relays": list(state_manager._online_relays.keys()),
                "online_switches": list(state_manager._online_switches.keys()),
                "up_to_date_devices": state_manager._up_to_date_devices,
            },
            "/rcm/ws/",
        )

        await asyncio.sleep(interval)


@mqtt.on_connect()
def connect(client, flags, rc, properties):
    mqtt.client.subscribe("/blind/pos")
    mqtt.client.subscribe("/heating/metrics")
    mqtt.client.subscribe("/relay/state/+")
    mqtt.client.subscribe("/switch/state/+")

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

    except ValueError as e:
        logger.error("Error processing relay state: %s", e)


async def handle_switch_state(payload_str, topic):
    """
    Process switch state payload from /switch/state/+ topic.
    """
    try:
        switch_id = topic.split("/")[-1]
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

    except ValueError as e:
        logger.error("Error processing switch state: %s", e)


async def handle_root_state(payload_str):
    """
    Process root switch state payload.
    """
    connections = connection_manager.connection_manager.get_all_connections()
    relays = connection_manager.connection_manager.get_relays()
    switches = connection_manager.connection_manager.get_switches()

    if payload_str == "connected":
        logger.debug("Connections: %s", connections)  # Debug log

        mqtt.client.publish("/switch/cmd/root", connections)

        return

    try:
        data = json.loads(payload_str)
        logger.debug("Root State Data: %s", data)  # Debug log

    except json.JSONDecodeError:
        logger.error("Error processing root state JSON payload: %s", payload_str)
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
            connection_manager.connection_manager.add_switch(data["deviceId"], name, 3)
            await ws_manager.broadcast({"type": "update"}, "/rcm/ws/")

    elif data["type"] == "relay":
        if data["deviceId"] in relays:
            data["name"] = relays[data["deviceId"]]
        else:
            name = namer.generate(category="animals")
            data["name"] = name
            connection_manager.connection_manager.add_relay(data["deviceId"], name)
            await ws_manager.broadcast({"type": "update"}, "/rcm/ws/")

    elif data["type"] == "root":
        data["name"] = "root"

        connection_manager.connection_manager.rootId = data["deviceId"]

    else:
        logger.warning(f"Unknown device type for ID {data['deviceId']}: {data['type']}")

    if "name" not in data:
        return

    state_manager.mark_relay_online(data["deviceId"], time())
    state_manager.mark_switch_online(data["deviceId"], time())
    state_manager.set_firmware_version(data["deviceId"], data["type"], data["firmware"])

    data["name"] = data["name"].replace(" ", "\\ ")

    if data["parentId"] in relays:
        parent_name = relays[data["parentId"]]

    elif data["parentId"] in switches:
        parent_name = switches[data["parentId"]][0]

    elif data["parentId"] == data["deviceId"] or data["parentId"] == connection_manager.connection_manager.rootId:
        parent_name = "root"

    else:
        parent_name = "unknown"

    data["parent_name"] = parent_name.replace(" ", "\\ ")

    if not config.monitoring.send_metrics:
        return

    metric_node = f"node_info,id={data['deviceId']},name={data['name']}{labels} uptime={data['uptime']},clicks={data['clicks']},free_heap={data['freeHeap']},disconnects={data['disconnects']}"
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
