import asyncio
import json
import logging
from string import ascii_lowercase

import httpx
import namer
from fastapi_mqtt import FastMQTT, MQTTConfig

from turbacz import connection_manager, metrics
from turbacz.settings import config
from turbacz.state import relay_state
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


@mqtt.on_connect()
def connect(client, flags, rc, properties):
    global task

    mqtt.client.subscribe("/blind/pos")
    mqtt.client.subscribe("/heating/metrics")
    mqtt.client.subscribe("/relay/state/+")
    mqtt.client.subscribe("/switch/state/+")

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

        await ws_manager.broadcast(data, "heating")

    except (json.JSONDecodeError, KeyError) as e:
        logger.error("Error processing heating metrics: %s", e)


async def handle_blind_position(payload_str):
    """
    Process blind position payload.
    """
    parts = payload_str.split()
    if len(parts) >= 2:
        data = {"blind": parts[0], "current_position": parts[1]}
        await ws_manager.broadcast(data, "blinds")
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

        relay_state.update_state(relay_id, output_id, state)
        print(f"Relay ID: {relay_id}, Output: {output_id}, State: {state}")

        data = {
            "type": "light_state",
            "relay_id": relay_id,
            "output_id": output_id,
            "state": state,
        }
        asyncio.create_task(ws_manager.broadcast(data, "/lights/ws/"))
        print(f"Published light state: {data}")

    except ValueError as e:
        logger.error("Error processing relay state: %s", e)


async def handle_switch_state(payload_str, topic):
    """
    Process switch state payload from /switch/state/+ topic.
    """
    try:
        switch_id = topic.split("/")[-1]
        light_id = payload_str[0]

        logger.debug("Switch ID: %s, Light ID: %s", switch_id, light_id)  # Debug log

        with open("turbacz/data/connections.json", "r", encoding="utf-8") as f:
            conf = json.load(f)
            connections = conf["connections"]
            outputs = connections[switch_id][light_id]

        logger.debug("Outputs: %s", outputs)  # Debug log

        for relay_id, output_id in outputs:
            logger.debug(
                "Relay ID: %s, Output ID: %s", relay_id, output_id
            )  # Debug log
            if len(payload_str) == 2:
                state = payload_str[1]

            mqtt.client.publish("/relay/cmd/" + relay_id, str(output_id) + state)

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

    except json.JSONDecodeError:
        logger.error("Invalid JSON payload for root state: %s", payload_str)
        return

    url = f"{config.monitoring.metrics}/api/v2/write"
    if config.monitoring.labels:
        labels = "," + ",".join(
            f"{key}={value}" for key, value in config.monitoring.labels.items()
        )
    else:
        labels = ""

    for dev_id, status in data.items():
        dev_id = int(dev_id)

        if status["type"] == "switch":
            if dev_id in switches:
                status["name"] = switches[dev_id][0]
            else:
                name = namer.generate(category="astronomy")
                status["name"] = name
                connection_manager.connection_manager.add_switch(dev_id, name, 3)
                await ws_manager.broadcast({"type": "update"}, "/rcm/ws/")

        elif status["type"] == "relay":
            if dev_id in relays:
                status["name"] = relays[dev_id][0]
            else:
                name = namer.generate(category="animals")
                status["name"] = name
                connection_manager.connection_manager.add_relay(dev_id, name)
                await ws_manager.broadcast({"type": "update"}, "/rcm/ws/")

        elif status["type"] == "root":
            status["name"] = "root"

        else:
            logger.warning(f"Unknown device type for ID {dev_id}: {status['type']}")

    for dev_id, status in data.items():
        if "name" not in status:
            continue

        status["name"] = status["name"].replace(" ", "\\ ")

        if status["parent"] != "0":
            try:
                status["parent_name"] = data[status["parent"]]["name"]
            except KeyError:
                status["parent_name"] = status["parent"]
        else:
            status["parent_name"] = "unknown"

        if not config.monitoring.send_metrics:
            continue

        metric_node = f"node_info,id={dev_id},name={status['name']}{labels} uptime={status['uptime']},clicks={status['clicks']},disconnects={status['disconnects']},last_seen={status['last_seen']}"
        metric_mesh = f"mesh_node,id={dev_id},name={status['name']},parent={status['parent']},parent_name={status['parent_name']},firmware={status['firmware']},status={status['status']},type={status['type']}{labels} rssi={status['rssi']}"

        if "free_heap" in status:
            metric_node += f",free_heap={status['free_heap']}"

        logger.debug(metric_node)  # Debug log
        logger.debug(metric_mesh)  # Debug log

        async with httpx.AsyncClient() as client:
            response = await client.post(url, content=metric_node)
            if response.status_code != 204:
                logger.error("Failed to write metric for %s: %s", dev_id, response.text)

            response = await client.post(url, content=metric_mesh)
            if response.status_code != 204:
                logger.error("Failed to write metric for %s: %s", dev_id, response.text)
