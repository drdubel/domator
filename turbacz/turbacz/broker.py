import json
import logging

from fastapi_mqtt import FastMQTT, MQTTConfig

from .websocket import ws_manager
from .secrets import mqtt_password

logger = logging.getLogger(__name__)
  

mqtt_config = MQTTConfig(
    host="localhost",
    port=1883,
    keepalive=60,
    username="turbacz",
    password=mqtt_password,
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
    await ws_manager.broadcast({"blind": payload[0:2], "current_position": payload[3:]})
