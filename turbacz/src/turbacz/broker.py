import json
import logging
import time
from string import ascii_uppercase, ascii_lowercase

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

switches = {
    '1' : 'przedpokoj', # (dcba)
    '2' : 'salon', # (cba)
    '3' : 'gabinet', # (dcba)
    '4' : 'komin dol', # (ab/dc/fe)
    '5' : 'lazienka dol', # (dc/ba)
    '9' : 'komin gora', # (abdcefg)
    '11' : 'lazienka gora', # (abcd)
    # przycisk przy drzwiach wyjsciowych nie dziala wiec nie wiem jaki ma ID
    # jeszcze nie sa zrobione:
    # - wyjscie na taras dol
    # - wyjscie na taras gora
    # - schody gora
}

# mapping of relays to their description and state
relays = {
    '1a': ('lazienka dol wentylator', 0),
    '1b': ('lazienka dol sufit', 0),
    '1c': ('brak', 0),
    '1d': ('kuchnia wywietrznik', 0),
    '1e': ('kuchnia komin', 0),
    '1f': ('kuchnia wyspa', 0),
    '1g': ('salon sufit halogeny', 0),
    '1h': ('brak', 0),
    '2a': ('przedpokoj sufit czwarte', 0),
    '2b': ('przedpokoj sufit trzy', 0),
    '2c': ('brak', 0),
    '2d': ('brak', 0),
    '2e': ('salon kinkiety', 0),
    '2f': ('brak', 0),
    '2g': ('brak', 0),
    '2h': ('brak', 0),
    '3a': ('lazienka gora pod prysznicem', 0),
    '3b': ('brak', 0),
    '3c': ('lazienka gora nad toaleta', 0),
    '3d': ('sypialnia sufit', 0),
    '3e': ('antresola kinkiety komin', 0),
    '3f': ('antresola halogeny', 0),
    '3g': ('salon korytarz', 0),
    '3h': ('gabinet sufit (rzedy zewnetrzne)', 0),
    # nie podpiete:
    # - przedpokoj nad lustrem
    # - lazienka dolna pod prysznicem
    # - gabinet sufit (rzedy wewnetrzne)
    # brak lamp wiec pewnie tez nie podpiete:
    # - lazienka dolna nad lustrem
    # - gabinet sciana od lazienki
    # - gabinet sciana od sasiadow
    # - sypialnia komin
    # - lazienka gorna wentylator
    # - wyjscie na taras
    # - taras podsufitka
}

# mapa switch: relay
mapping = {
        '2a': '1e', # salon > kuchnia komin
        '2c': '2e', # salon > kinkiety
        '5d': '1b', # lazienka dol > sufit
        '1a': '2b', # przedpokoj > sufit trzy
        '1b': '2a', # przedpokoj > sufit czwarte
        '11a': '3c', # lazienka gora > nad toaleta 
        '11b': '3a', # lazienka gora > pod prysznicem
        '3a': '3h', # gabinet > sufit
        '4a': '1e', # komin > kuchnia wyspa
        '4b': '1f', # komin > kuchnia komin
        '4e': '1d', # komin > kuchnia wywietrznik
        }


@mqtt.on_connect()
def connect(client, flags, rc, properties):
    mqtt.client.subscribe("/switch/#")
    # all relays are in three blue relay boxes, but that's mostly irrelevant
    # other than that they report state on different (per relay box) topics
    mqtt.client.subscribe("/relay/1/state")
    mqtt.client.subscribe("/relay/2/state")
    mqtt.client.subscribe("/relay/3/state")
    logger.info("Connected: %s %s %s %s", client, flags, rc, properties)


@mqtt.on_message()
async def message(client, topic, payload, qos, properties):
    payload = payload.decode()

    if topic.startswith("/switch/") and payload == "connected":
        switch_id = topic.split("/")[2]
        switch_desc = switches.get(switch_id, "unknown")
        logger.info(f'Switch {switch_id} ({switch_desc}) connected')

    elif topic.startswith("/switch/"):
        switch_id = topic.split("/")[2]
        switch_desc = switches.get(switch_id, "unknown")
        button = payload[0].lower()
        button_id = switch_id + button

        if button_id in mapping:
            relay = mapping[button_id]
            if relay not in relays:
                logger.error(f'Switch {button_id} ({switch_desc}) mapped to unknown relay {relay}')
                return
            relay_desc, relay_state = relays[relay]
        else:
            logger.info(f'Button {button_id} ({switch_desc}): not yet mapped to light')
            return

        logger.info(f"Switch {button_id} ({switch_desc}) -> relay {relay} ({relay_desc}) / state {relay_state} -> {relay_state^1}")
        # note: it'll break if there's more than 9 blue relay boxes
        relay_id, bus = relay[0], relay[1]
        mqtt.client.publish(f"/relay/{relay_id}/cmd", f"{bus}{relay_state^1}")

    elif topic.startswith("/relay/"):
        relay_id = topic.split("/")[2]
        bus_id = payload[0].lower()
        relay = relay_id + bus_id
        if relay not in relays:
            logger.error(f'Unknown relay {relay} state received {payload}')
            return
        relay_desc, relay_saved_state = relays[relay]
        relay_actual_state = int(payload[1])
        relays[relay] = relay_desc, relay_actual_state

        payload = {
            "id": relay,
            "state": relay_actual_state,
        }

        logger.info(f"Relay {relay} ({relay_desc}) state {relay_actual_state} (old {relay_saved_state})")

        # broadcast to clients so they update their UIs (view of lights)
        await ws_manager.broadcast(payload, "lights")

    else:
        logger.debug(f"Received unknown {topic}: {payload}")
