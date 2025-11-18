import json
import logging
import time
from string import ascii_lowercase, ascii_uppercase

from fastapi_mqtt import FastMQTT, MQTTConfig

from . import metrics
from .data.secrets import mqtt_password
from .websocket import ws_manager

logger = logging.getLogger(__name__)


mqtt_config = MQTTConfig(
    host="127.0.0.1",
    port=1883,
    keepalive=60,
    username="torpeda",
    password=mqtt_password,
)

mqtt = FastMQTT(config=mqtt_config)


lights = [[0, 0, 0, 0, 0, 0, 0, 0], [0, 0, 0, 0, 0, 0, 0, 0], [0, 0, 0, 0, 0, 0, 0, 0]]


def switch2relay(switchId, button):
    return ("1", button)


@mqtt.on_connect()
def connect(client, flags, rc, properties):
    mqtt.client.subscribe("/switch/#")
    mqtt.client.subscribe("/relay/1/state")
    mqtt.client.subscribe("/relay/2/state")
    mqtt.client.subscribe("/relay/3/state")

    logger.info("Connected: %s %s %s %s", client, flags, rc, properties)


@mqtt.on_message()
async def message(client, topic, payload, qos, properties):
    payload = payload.decode()
    print(f"Received message on topic {topic}: {payload}")
    if topic.startswith("/switch/"):
        switchId = topic.split("/")[2]
        payload = payload[0]

        relay = switch2relay(switchId, payload)
        lightId = ascii_lowercase.index(relay[1])

        print(f"Switch {switchId} button {payload} -> relay {relay}")
        print(relay[1] + str(lights[int(relay[0]) - 1][lightId] ^ 1))

        mqtt.client.publish(
            f"/relay/{relay[0]}/cmd",
            relay[1] + str(lights[int(relay[0]) - 1][lightId] ^ 1),
        )

    elif topic.startswith("/relay/"):
        relayId = topic.split("/")[2]

        lights[int(relayId) - 1][ascii_uppercase.index(payload[0])] = int(payload[1])
        payload = {
            "id": relayId + payload[0].lower(),
            "state": int(payload[1]),
        }

        print(payload)

        await ws_manager.broadcast(payload, "lights")
