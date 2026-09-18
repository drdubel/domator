"""Shared MQTT client.

Kept in its own module so that both :mod:`turbacz.broker` (which owns the
``on_connect``/``on_message`` handlers) and :mod:`turbacz.ha.bridge` (which
publishes Home Assistant topics) can reach the client without importing each
other.
"""

from fastapi_mqtt import FastMQTT, MQTTConfig
from gmqtt import Message

from turbacz.settings import config

mqtt_config = MQTTConfig(
    host=config.mqtt.host,
    port=config.mqtt.port,
    keepalive=60,
    username=config.mqtt.username,
    password=config.mqtt.password,
)

mqtt = FastMQTT(config=mqtt_config)

# MQTTConfig has no will_message_retain field and fastapi_mqtt builds the will
# without one, so set it directly. A non-retained LWT would leave Home
# Assistant showing stale states after a turbacz crash, because an HA instance
# subscribing later never sees the "offline" message.
if config.ha.enabled:
    mqtt.client._will_message = Message(
        f"{config.ha.base_topic}/status",
        "offline",
        qos=1,
        retain=True,
        will_delay_interval=0,
    )


def publish_blind_action(client, relay_id: int, power_id: str, direction_id: str, action: str) -> bool:
    """Drive a blind pair. Returns False for an unrecognised action.

    direction OFF (0) = up, direction ON (1) = down; power ON (1) starts the
    motor. Shared by the /blinds WebSocket and the Home Assistant bridge so the
    two command paths cannot drift apart.
    """
    if action == "up":
        client.publish(f"/relay/cmd/{relay_id}", f"{direction_id}0")
        client.publish(f"/relay/cmd/{relay_id}", f"{power_id}1")
    elif action == "down":
        client.publish(f"/relay/cmd/{relay_id}", f"{direction_id}1")
        client.publish(f"/relay/cmd/{relay_id}", f"{power_id}1")
    elif action == "stop":
        client.publish(f"/relay/cmd/{relay_id}", f"{power_id}0")
    else:
        return False

    return True
