"""Discovery, routing isolation, and protocol subtopics under zigbee2mqtt/#."""

import json

import pytest

from turbacz.zigbee_devices import zigbee_device_id, zigbee_topic

from .test_security import application as application  # noqa: PLC0414
from .test_zigbee_knob import knob as knob  # noqa: PLC0414


@pytest.mark.parametrize(
    "topic",
    [
        "zigbee2mqtt/bridge/state",
        "zigbee2mqtt/bridge/logging",
        "zigbee2mqtt/bridge/response/device/remove",
        "zigbee2mqtt/button/set",
        "zigbee2mqtt/light/set/state",
        "zigbee2mqtt/button/get",
        "zigbee2mqtt/light/get/state",
        "zigbee2mqtt/button/action",
    ],
)
async def test_protocol_endpoints_do_not_create_cards(knob, application, topic):
    _, cm, _ = knob
    from turbacz import broker

    await broker.message(None, topic, b'{"action":"single"}', 0, {})
    cm.register_zigbee_device.assert_not_called()
    cm.get_all_connections.assert_not_called()
    assert not application.mqtt.sent


async def test_each_device_has_its_own_card_and_independent_action_commands(
    knob, application
):
    zigbee, cm, _ = knob
    first = "zigbee2mqtt/first_remote"
    second = "zigbee2mqtt/second_remote"
    await zigbee.handle_zigbee_message(first, '{"action":"single"}')
    await zigbee.handle_zigbee_message(second, '{"action":"single"}')
    first_id, second_id = zigbee_device_id(first), zigbee_device_id(second)
    assert first_id != second_id
    devices = cm.get_zigbee_devices()
    assert devices[first_id]["topic"] == first
    assert devices[second_id]["topic"] == second
    assert len(devices[first_id]["buttons"]) == len(devices[second_id]["buttons"]) == 1
    cm.set_zigbee_command(first_id, "a", "on")
    cm.set_zigbee_command(second_id, "a", "off")
    cm.get_all_connections.return_value = {
        first_id: {"a": [(111, "a")]},
        second_id: {"a": [(111, "b")]},
    }
    await zigbee.handle_zigbee_message(first, '{"action":"single"}')
    await zigbee.handle_zigbee_message(second, '{"action":"single"}')
    assert application.mqtt.drain() == [
        ("/relay/cmd/111", "a1", False),
        ("/relay/cmd/111", "b0", False),
    ]


async def test_sensor_reports_discover_a_card_without_fake_action_buttons(
    knob, application
):
    zigbee, cm, _ = knob
    topic = "zigbee2mqtt/kitchen/sensor"
    await zigbee.handle_zigbee_message(
        topic, '{"temperature":21.5,"battery":90}', retained=True
    )
    sensor = cm.get_zigbee_devices()[zigbee_device_id(topic)]
    assert sensor["topic"] == topic
    assert sensor["actions"] == sensor["buttons"] == {}
    cm.get_all_connections.assert_not_called()
    assert not application.mqtt.sent


@pytest.mark.parametrize("payload", ['{"state":"online"}', '"offline"', "online"])
async def test_availability_belongs_to_the_device_and_does_not_execute_actions(
    knob, application, payload
):
    zigbee, cm, _ = knob
    topic = "zigbee2mqtt/kitchen/sensor"
    await zigbee.handle_zigbee_message(topic + "/availability", payload, retained=True)
    assert cm.get_zigbee_devices()[zigbee_device_id(topic)]["topic"] == topic
    cm.register_zigbee_device.assert_called_once_with(topic, [])
    cm.get_all_connections.assert_not_called()
    assert not application.mqtt.sent


async def test_retained_action_discovers_device_without_replaying_output(
    knob, application
):
    zigbee, cm, _ = knob
    from turbacz import broker

    topic = "zigbee2mqtt/bedroom/remote"
    await broker.message(None, topic, b'{"action":"1_single"}', 0, {"retain": 1})
    device = cm.get_zigbee_devices()[zigbee_device_id(topic)]
    assert device["actions"] == {"1_single": "a"}
    zigbee.ha_bridge.on_button_event.assert_not_awaited()
    cm.get_all_connections.assert_not_called()
    assert not application.mqtt.sent


async def test_bridge_inventory_discovers_actual_devices_and_advertised_actions(
    knob, application
):
    zigbee, cm, _ = knob
    from turbacz import broker

    inventory = [
        {"friendly_name": "Coordinator", "type": "Coordinator"},
        {
            "friendly_name": "bedroom/knob",
            "type": "EndDevice",
            "definition": {
                "exposes": [
                    {
                        "property": "action",
                        "values": ["single", "rotate_left", "rotate_right"],
                    }
                ]
            },
        },
        {"friendly_name": "kitchen/sensor", "type": "EndDevice", "definition": None},
    ]
    await broker.message(
        None,
        "zigbee2mqtt/bridge/devices",
        json.dumps(inventory).encode(),
        0,
        {"retain": 1},
    )
    registered = [call.args[0] for call in cm.register_zigbee_device.call_args_list]
    assert registered == ["zigbee2mqtt/bedroom/knob", "zigbee2mqtt/kitchen/sensor"]
    device = cm.get_zigbee_devices()[zigbee_device_id(registered[0])]
    assert set(device["actions"]) == {"single", "rotate_left", "rotate_right"}
    zigbee.ws_manager.broadcast.assert_awaited_once_with({"type": "update"}, "/rcm/ws/")
    cm.get_all_connections.assert_not_called()
    assert not application.mqtt.sent


async def test_empty_bridge_inventory_creates_nothing(knob, application):
    zigbee, cm, _ = knob
    await zigbee.handle_zigbee_message(
        "zigbee2mqtt/bridge/devices", "[]", retained=True
    )
    cm.register_zigbee_device.assert_not_called()
    zigbee.ws_manager.broadcast.assert_not_awaited()
    assert not application.mqtt.sent


@pytest.mark.parametrize(
    "payload", ["{}", "[null]", "[{}]", '[{"friendly_name":"lamp","definition":[]}]']
)
async def test_malformed_inventory_is_validated_before_discovery(knob, payload):
    _, cm, _ = knob
    from turbacz import broker

    await broker.message(
        None, "zigbee2mqtt/bridge/devices", payload.encode(), 0, {"retain": 1}
    )
    cm.register_zigbee_device.assert_not_called()


def test_topic_identity_preserves_folder_names_and_is_javascript_safe():
    assert zigbee_topic("zigbee2mqtt/room/knob") == ("zigbee2mqtt/room/knob", "state")
    assert zigbee_topic("zigbee2mqtt/room/knob/availability") == (
        "zigbee2mqtt/room/knob",
        "availability",
    )
    first = zigbee_device_id("zigbee2mqtt/room/knob")
    assert first == zigbee_device_id("zigbee2mqtt/room/knob")
    assert first != zigbee_device_id("zigbee2mqtt/another_knob")
    assert 2**48 <= first < 2**49 < 2**53
