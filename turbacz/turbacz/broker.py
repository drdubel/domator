import asyncio
import json
import logging
from string import ascii_lowercase
from time import perf_counter_ns

import httpx
import namer
from fastapi_mqtt import FastMQTT, MQTTConfig

from turbacz import metrics
from turbacz.data.secrets import mqtt_password
from turbacz.state import relay_state
from turbacz.websocket import ws_manager

logger = logging.getLogger(__name__)


mqtt_config = MQTTConfig(
    host="127.0.0.1",
    port=1883,
    keepalive=60,
    username="turbacz",
    password=mqtt_password,
)

mqtt = FastMQTT(config=mqtt_config)
task: asyncio.Task | None = None


def process_connections(connections):
    """
    Process connections from configuration file.
    """
    processed = {}
    for switch_id in connections:
        processed[switch_id[:10]] = {}
        for button_id in connections[switch_id]:
            processed[switch_id[:10]][button_id] = []
            outputs = connections[switch_id][button_id]
            for i in range(len(outputs)):
                relay_id, output_id = outputs[i]
                outputs[i] = (str(relay_id), str(output_id))
                processed[switch_id[:10]][button_id] = outputs

    logger.debug("Processed connections: %s", processed)  # Debug log
    return processed


@mqtt.on_connect()
def connect(client, flags, rc, properties):
    global task

    mqtt.client.subscribe("/blind/pos")
    mqtt.client.subscribe("/heating/metrics")
    mqtt.client.subscribe("/switch/1/state")
    mqtt.client.subscribe("/relay/state/+")
    mqtt.client.subscribe("/switch/state/+")

    # if task is None:
    #     task = asyncio.create_task(get_delays())

    logger.info("Connected: %s %s %s %s", client, flags, rc, properties)


@mqtt.on_disconnect()
async def on_disconnect(client, packet, exc=None):
    global task

    if task:
        task.cancel()
        task = None


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

    elif topic == "/switch/1/state":
        await handle_old_switch_state(payload_str)

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


async def handle_old_switch_state(payload_str):
    """
    Process old switch state payload.
    """
    try:
        if len(payload_str) >= 2:
            switch_id = "s" + str(ascii_lowercase.index(payload_str[0]))
            state = int(payload_str[1])
            data = {"id": switch_id, "state": state}

            relay_state.update_state(switch_id, state)

            asyncio.create_task(ws_manager.broadcast(data, "lights"))
        else:
            logger.warning("Invalid switch state payload: %s", payload_str)

    except (ValueError, IndexError) as e:
        logger.error("Error processing switch state: %s", e)


async def handle_relay_state(payload_str, topic):
    """
    Process switch state payload from /relay/state/+ topic.
    """
    try:
        relay_id = topic.split("/")[-1]

        if payload_str == "P":
            # Handle ping message
            relay_state.update_ping_time(relay_id, perf_counter_ns())
            logger.debug(
                "Ping from relay: %s %s %s",
                relay_id,
                relay_state.get_ping_time(relay_id=relay_id),
            )  # Debug log
            return

        state = int(payload_str[1])
        idx = ord(payload_str[0]) - ord("A")
        light_id = relay_state.route_lights(relay_id, idx)

        relay_state.update_state(light_id, state)

        data = {"id": light_id, "state": state}
        asyncio.create_task(ws_manager.broadcast(data, "lights"))

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

    try:
        with open("turbacz/data/connections.json", "r", encoding="utf-8") as f:
            conf = json.load(f)
            connections = conf["connections"]
            names = conf["deviceNames"]

    except KeyError as e:
        logger.error("Error processing connections: %s", e)

        return

    if payload_str == "connected":
        logger.debug("Connections: %s", connections)  # Debug log
        processed_connections = process_connections(connections)

        mqtt.client.publish("/switch/cmd/root", processed_connections)
        mqtt.client.publish("/switch/1/cmd", "S")
        mqtt.client.publish("/relay/cmd/1074130365", "S")
        mqtt.client.publish("/relay/cmd/1074122133", "S")

        return

    try:
        data = json.loads(payload_str)

    except json.JSONDecodeError:
        logger.error("Invalid JSON payload for root state: %s", payload_str)
        return

    url = "http://192.168.3.10:8428/api/v2/write"

    for switch_id, status in data.items():
        if switch_id in names:
            status["name"] = names[switch_id]
        elif status["type"] == "root":
            status["name"] = "root"
        else:
            status["name"] = namer.generate(category="astronomy")

    for switch_id, status in data.items():
        status["name"] = status["name"].replace(" ", "\\ ")

        if status["parent"] != "0":
            try:
                status["parent_name"] = data[status["parent"]]["name"]
            except KeyError:
                status["parent_name"] = status["parent"]
        else:
            status["parent_name"] = "unknown"

        metric_node = f"node_info,id={switch_id},name={status['name']} uptime={status['uptime']},clicks={status['clicks']},disconnects={status['disconnects']},last_seen={status['last_seen']}"
        metric_mesh = f"mesh_node,id={switch_id},name={status['name']},parent={status['parent']},parent_name={status['parent_name']},firmware={status['firmware']},status={status['status']},type={status['type']} rssi={status['rssi']}"

        if "free_heap" in status:
            metric_node += f",free_heap={status['free_heap']}"

        logger.debug(metric_node)  # Debug log
        logger.debug(metric_mesh)  # Debug log

        async with httpx.AsyncClient() as client:
            response = await client.post(url, content=metric_node)
            if response.status_code != 204:
                logger.error(
                    "Failed to write metric for %s: %s", switch_id, response.text
                )

            response = await client.post(url, content=metric_mesh)
            if response.status_code != 204:
                logger.error(
                    "Failed to write metric for %s: %s", switch_id, response.text
                )

    with open("turbacz/data/connections.json", "r", encoding="utf-8") as f:
        try:
            conf = json.load(f)

        except json.JSONDecodeError:
            logger.error("Invalid JSON in connections file.")
            return

        connections = conf["connections"]
        names = conf["deviceNames"]
        names.update(
            {switch_id: data[switch_id]["name"].replace("\\", "") for switch_id in data}
        )
        conf["deviceNames"] = names

    with open("turbacz/data/connections.json", "w", encoding="utf-8") as f:
        json.dump(conf, f, ensure_ascii=False, indent=4)


async def get_delays():
    while True:
        for relay in relay_state.relays:
            logger.debug("Pinging relay: %s", relay)  # Debug log
            mqtt.publish(f"/relay/cmd/{relay}", "P", qos=1)
            relay_state.update_send_ping_time(relay, perf_counter_ns())

            await asyncio.sleep(0.2)  # seconds

        await asyncio.sleep(3)  # seconds
