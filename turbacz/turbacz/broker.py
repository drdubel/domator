import json
import logging
import time

from fastapi_mqtt import FastMQTT, MQTTConfig

from . import metrics
from .data.secrets import mqtt_password
from .websocket import ws_manager

logger = logging.getLogger(__name__)


mqtt_config = MQTTConfig(
    host="127.0.0.1",
    port=1883,
    keepalive=60,
    username="turbacz",
    password=mqtt_password,
)

mqtt = FastMQTT(config=mqtt_config)


@mqtt.on_connect()
def connect(client, flags, rc, properties):
    mqtt.client.subscribe("/blind/pos")
    mqtt.client.subscribe("/heating/metrics")
    mqtt.publish("/blind/cmd", "S")
    logger.info("Connected: %s %s %s %s", client, flags, rc, properties)


@mqtt.on_message()
async def message(client, topic, payload, qos, properties):
    payload = payload.decode()
    if topic == "/heating/metrics":
        payload = json.loads(payload)
        for probe in ("cold", "mixed", "hot"):
            metrics.water_temp.set({"probe": probe}, payload[probe])
        metrics.pid_integral.set({}, payload["integral"])
        metrics.pid_output.set({}, payload["pid_output"])
        metrics.pid_target.set({}, payload["target"])
        for multiplier in ("kp", "ki", "kd"):
            metrics.pid_multiplier.set({"multiplier": multiplier}, payload[multiplier])
        with open("./static/data/heating_chart.json", "r") as chart_data_json:
            chart_data = json.load(chart_data_json)
            if len(chart_data["labels"]) == 1000:
                chart_data["labels"] = chart_data["labels"][1:]
                chart_data["cold"] = chart_data["cold"][1:]
                chart_data["mixed"] = chart_data["mixed"][1:]
                chart_data["hot"] = chart_data["hot"][1:]
            chart_data["labels"].append(
                time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
            )
            chart_data["cold"].append(payload["cold"])
            chart_data["mixed"].append(payload["mixed"])
            chart_data["hot"].append(payload["hot"])
        with open("./static/data/heating_chart.json", "w") as chart_data_json:
            json.dump(chart_data, chart_data_json)
        await ws_manager.broadcast(payload, "heating")
    elif topic == "/blind/pos":
        await ws_manager.broadcast(
            {"blind": payload[0], "current_position": payload[1]}, "blinds"
        )
