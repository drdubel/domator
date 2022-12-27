import logging

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
    payload = payload.decode().split()
    if topic == "/heating/metrics":
        await ws_manager.broadcast({"id": payload[0], "value": payload[1]})
    elif topic == "/blind/pos":
        await ws_manager.broadcast(
            {"blind": payload[0], "current_position": payload[1]}
        )
