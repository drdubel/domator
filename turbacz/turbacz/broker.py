import json
import logging

from fastapi_mqtt import FastMQTT, MQTTConfig

from .websocket import ws_manager

logger = logging.getLogger(__name__)


mqtt_config = MQTTConfig(
    host="127.0.0.1",
    port=1883,
    keepalive=60,
    username="aplikacja_webowa",
    password="password",
)

mqtt = FastMQTT(config=mqtt_config)


@mqtt.on_connect()
def connect(client, flags, rc, properties):
    mqtt.client.subscribe("/blind/pos")  # subscribing mqtt topic
    mqtt.publish("/blind/cmd", "S")
    logger.info("Connected: %s %s %s %s", client, flags, rc, properties)


@mqtt.on_message()
async def message(client, topic, payload, qos, properties):
    payload = json.loads(payload.decode())
    logger.debug("Received message: %s %s %s %s", topic, payload, qos, properties)
    logger.debug("%s %s", type(payload), payload)
    for blind, pos in payload.items():
        await ws_manager.broadcast({"blind": blind, "current_position": pos})
