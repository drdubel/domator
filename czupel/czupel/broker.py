import json
import logging
import time
from string import ascii_lowercase

from fastapi_mqtt import FastMQTT, MQTTConfig

from czupel import metrics
from czupel.data.secrets import mqtt_password
from czupel.websocket import ws_manager

logger = logging.getLogger(__name__)


mqtt_config = MQTTConfig(
    host="127.0.0.1",
    port=1883,
    keepalive=60,
    username="turbacz",
    password=mqtt_password,
)

mqtt = FastMQTT(config=mqtt_config)


route_lights = {"1074130365": ["s9", "s10", "s11", "s12", "s13", "s14", "s15", "s16"]}


@mqtt.on_connect()
def connect(client, flags, rc, properties):
    mqtt.client.subscribe("/blind/pos")
    mqtt.client.subscribe("/heating/metrics")
    mqtt.client.subscribe("/switch/1/state")
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
    elif topic == "/switch/1/state":
        await handle_old_switch_state(payload_str)
    elif topic.startswith("/relay/state/"):
        await handle_relay_state(payload_str, topic)
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

        # Update chart data
        chart_file = "./static/data/heating_chart.json"
        with open(chart_file, "r") as f:
            chart_data = json.load(f)

        # Maintain max 1000 entries
        if len(chart_data["labels"]) == 1000:
            for key in ["labels", "cold", "mixed", "hot", "target"]:
                chart_data[key] = chart_data[key][1:]

        # Append new data
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
        chart_data["labels"].append(timestamp)
        chart_data["cold"].append(data["cold"])
        chart_data["mixed"].append(data["mixed"])
        chart_data["hot"].append(data["hot"])
        chart_data["target"].append(data["target"])

        with open(chart_file, "w") as f:
            json.dump(chart_data, f)

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
        state = int(payload_str[1])
        idx = ord(payload_str[0]) - ord("A")
        light_id = route_lights[relay_id][idx]

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

        if switch_id == "root":
            return

        light_id = payload_str[0]

        print(switch_id, light_id)  # Debug print

        with open("czupel/data/connections.json", "r", encoding="utf-8") as f:
            conf = json.load(f)
            connections = conf["connections"]
            outputs = connections[switch_id][light_id]

        print(outputs)  # Debug print

        for relay_id, output_id in outputs:
            print(relay_id, output_id)  # Debug print
            mqtt.client.publish("/relay/cmd/" + relay_id, str(output_id))

    except ValueError as e:
        logger.error("Error processing switch state: %s", e)
