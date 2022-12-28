import logging
import json

import time
from fastapi_mqtt import FastMQTT, MQTTConfig

from .secrets import mqtt_password
from .websocket import ws_manager

logger = logging.getLogger(__name__)


mqtt_config = MQTTConfig(
    host="192.168.3.10",
    port=1883,
    keepalive=60,
    username="czupel",
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
    payload = json.loads(payload.decode())
    if topic == "/heating/metrics":
        with open("./static/data/heating_chart.json", "r") as chart_data_json:
            chart_data = json.load(chart_data_json)
            chart_data["labels"].append(
                time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
            )
            chart_data["cold"].append(payload["cold"])
            chart_data["mixed"].append(payload["mixed"])
            chart_data["hot"].append(payload["hot"])
        with open("./static/data/heating_chart.json", "w") as chart_data_json:
            json.dump(chart_data, chart_data_json)
        await ws_manager.broadcast(payload)
    elif topic == "/blind/pos":
        await ws_manager.broadcast(
            {"blind": payload[0], "current_position": payload[1]}
        )
