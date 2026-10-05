"""RCM-configured Zigbee actions control outputs without a mesh button event."""

import json
from unittest.mock import AsyncMock, MagicMock

import pytest
from pydantic import ValidationError
from starlette.websockets import WebSocketDisconnect

from turbacz.settings import ZigbeeKnobSettings, config
from turbacz.zigbee_knob import (
    KNOB_ACTION_BUTTONS,
    KNOB_BUTTONS,
    KNOB_SWITCH_ID,
    mesh_switch_config,
)

from .test_security import application as application  # noqa: PLC0414


@pytest.fixture
def knob(application, monkeypatch):
    from turbacz import zigbee

    cm = application.cm
    cm.switches[KNOB_SWITCH_ID] = ("tyua_knob", len(KNOB_BUTTONS))
    choices = {
        "enabled": True,
        "switch_id": KNOB_SWITCH_ID,
        "buttons": {
            button: {"label": label, "command": default}
            for button, (label, default) in KNOB_BUTTONS.items()
        },
    }
    cm.get_zigbee_knob_config = MagicMock(return_value=choices)
    cm.get_all_connections = MagicMock(return_value={})

    def save(button_id, command):
        choices["buttons"][button_id]["command"] = command

    cm.set_zigbee_knob_command = MagicMock(side_effect=save)
    monkeypatch.setattr(zigbee, "connection_manager", cm)
    monkeypatch.setattr(zigbee.ws_manager, "broadcast", AsyncMock())
    monkeypatch.setattr(zigbee.ha_bridge, "on_button_event", AsyncMock())
    monkeypatch.setattr(config.zigbee, "tyua_knob", ZigbeeKnobSettings())
    return zigbee, cm, choices


@pytest.mark.parametrize("action,button", KNOB_ACTION_BUTTONS.items())
async def test_event_and_command_mode_actions_use_rcm_connections(
    knob, application, action, button
):
    zigbee, cm, choices = knob
    choices["buttons"][button]["command"] = "toggle"
    cm.get_all_connections.return_value = {
        KNOB_SWITCH_ID: {button: [(111, "a"), (111, "b")]}
    }
    from turbacz import broker

    await broker.message(
        None, "zigbee2mqtt/tyua_knob", json.dumps({"action": action}).encode(), 0, {}
    )
    assert application.mqtt.drain() == [
        ("/relay/cmd/111", "a", False),
        ("/relay/cmd/111", "b", False),
    ]
    zigbee.ws_manager.broadcast.assert_awaited_once_with(
        {"type": "switch_state", "switch_id": KNOB_SWITCH_ID, "button_id": button},
        "/rcm/ws/",
    )
    zigbee.ha_bridge.on_button_event.assert_awaited_once_with(
        KNOB_SWITCH_ID, button, "press"
    )


@pytest.mark.parametrize(
    "command,expected", [("toggle", "a"), ("on", "a1"), ("off", "a0")]
)
async def test_light_command_selection(knob, application, command, expected):
    zigbee, cm, choices = knob
    cm.get_all_connections.return_value = {KNOB_SWITCH_ID: {"a": [(111, "a")]}}
    choices["buttons"]["a"]["command"] = command
    await zigbee.handle_knob_message('{"action":"single"}')
    assert application.mqtt.drain() == [("/relay/cmd/111", expected, False)]


@pytest.mark.parametrize(
    "command,expected", [("up", ["d0", "c1"]), ("down", ["d1", "c1"]), ("stop", ["c0"])]
)
async def test_blind_pair_order_and_duplicate_connections(
    knob, application, command, expected
):
    zigbee, cm, choices = knob
    choices["buttons"]["a"]["command"] = command
    # Connecting both members must still execute the pair just once.
    cm.get_all_connections.return_value = {
        KNOB_SWITCH_ID: {"a": [(111, "d"), (111, "c")]}
    }
    await zigbee.handle_knob_message('{"action":"single"}')
    assert application.mqtt.drain() == [
        ("/relay/cmd/111", payload, False) for payload in expected
    ]


@pytest.mark.parametrize(
    "command,targets",
    [
        ("on", [(111, "a"), (111, "z")]),
        ("toggle", [(111, "a"), (111, "c")]),
        ("up", [(111, "c"), (111, "a")]),
        ("off", [(999, "a")]),
    ],
)
async def test_invalid_or_incompatible_targets_never_partially_execute(
    knob, application, command, targets
):
    _, cm, choices = knob
    choices["buttons"]["a"]["command"] = command
    cm.get_all_connections.return_value = {KNOB_SWITCH_ID: {"a": targets}}
    from turbacz import broker

    await broker.message(
        None, config.zigbee.tyua_knob.topic, b'{"action":"single"}', 0, {}
    )
    assert not application.mqtt.sent


@pytest.mark.parametrize(
    "payload", [b"bad", b"[]", b"null", b'{"action":true}', b'{"action":[]}', b"\xff"]
)
async def test_malformed_payload_has_no_effects(knob, application, payload):
    zigbee, cm, _ = knob
    from turbacz import broker

    await broker.message(None, config.zigbee.tyua_knob.topic, payload, 0, {})
    cm.get_all_connections.assert_not_called()
    zigbee.ws_manager.broadcast.assert_not_awaited()
    assert not application.mqtt.sent


@pytest.mark.parametrize(
    "payload",
    [
        b"{}",
        b'{"battery":95}',
        b'{"action":null}',
        b'{"action":""}',
        b'{"action":"unknown"}',
    ],
)
async def test_telemetry_and_action_resets_are_ignored(knob, application, payload):
    zigbee, cm, _ = knob
    from turbacz import broker

    await broker.message(None, config.zigbee.tyua_knob.topic, payload, 0, {})
    cm.get_all_connections.assert_not_called()
    zigbee.ws_manager.broadcast.assert_not_awaited()
    assert not application.mqtt.sent


async def test_retained_and_disabled_messages_cannot_replay_action(knob, application):
    zigbee, cm, _ = knob
    from turbacz import broker

    await broker.message(
        None, config.zigbee.tyua_knob.topic, b'{"action":"single"}', 0, {"retain": 1}
    )
    config.zigbee.tyua_knob.enabled = False
    await broker.message(
        None, config.zigbee.tyua_knob.topic, b'{"action":"single"}', 0, {}
    )
    cm.get_all_connections.assert_not_called()
    zigbee.ws_manager.broadcast.assert_not_awaited()
    assert not application.mqtt.sent


async def test_custom_topic_and_unconnected_actions(knob, application):
    _, cm, _ = knob
    config.zigbee.tyua_knob.topic = "zigbee2mqtt/another_knob"
    from turbacz import broker

    await broker.message(None, "zigbee2mqtt/tyua_knob", b'{"action":"single"}', 0, {})
    cm.get_all_connections.assert_not_called()
    await broker.message(
        None, config.zigbee.tyua_knob.topic, b'{"action":"single"}', 0, {}
    )
    cm.get_all_connections.assert_called_once()
    assert not application.mqtt.sent


@pytest.mark.parametrize("enabled", [True, False])
async def test_connect_subscribes_to_configured_topic(
    application, monkeypatch, enabled
):
    from turbacz import broker

    monkeypatch.setattr(
        config.zigbee,
        "tyua_knob",
        ZigbeeKnobSettings(enabled=enabled, topic="zigbee2mqtt/custom"),
    )
    monkeypatch.setattr(config.ha, "enabled", False)
    monkeypatch.setattr(application.mqtt, "subscribe", MagicMock(), raising=False)
    monkeypatch.setattr(broker, "periodic_check_devices", AsyncMock())
    broker.connect(None, None, 0, {})
    subscriptions = [call.args[0] for call in application.mqtt.subscribe.call_args_list]
    assert ("zigbee2mqtt/custom" in subscriptions) == enabled
    await broker.stop_background_tasks()


def test_rcm_configuration_requires_authentication(knob, application):
    _, _, choices = knob
    assert application.http.get("/lights/get_zigbee_knob_config").status_code == 401
    response = application.http.get(
        "/lights/get_zigbee_knob_config",
        headers={"Authorization": f"Bearer {application.token}"},
    )
    assert response.json() == choices


@pytest.mark.parametrize(
    "button,command", [("z", "on"), ("a", "dim"), ("a", []), (None, "on")]
)
def test_invalid_rcm_command_is_rejected(knob, button, command):
    from turbacz.validation import validate_command

    _, cm, _ = knob
    with pytest.raises(ValueError):
        validate_command(
            "/rcm/ws/1",
            {"type": "zigbee_knob_command", "button_id": button, "command": command},
            cm,
        )


def test_rcm_command_is_saved_and_broadcast_to_clients(knob, application, monkeypatch):
    zigbee, cm, choices = knob
    manager = zigbee.ws_manager
    monkeypatch.setattr(manager, "broadcast", type(manager).broadcast.__get__(manager))
    with application.http.websocket_connect(
        f"/rcm/ws/1?token={application.token}"
    ) as ws:
        assert ws.receive_json()["type"] == "online_status"
        ws.send_json({"type": "zigbee_knob_command", "button_id": "d", "command": "up"})
        result = ws.receive_json()
        assert result["type"] == "zigbee_knob_config"
        assert result["buttons"]["d"]["command"] == "up"
    cm.set_zigbee_knob_command.assert_called_once_with("d", "up")
    response = application.http.get(
        "/lights/get_zigbee_knob_config",
        headers={"Authorization": f"Bearer {application.token}"},
    )
    assert response.json() == choices


def test_operators_cannot_change_knob_routing(knob, application, monkeypatch):
    _, cm, _ = knob
    monkeypatch.setitem(config.roles, "test@example.com", {"operator"})
    with application.http.websocket_connect(
        f"/rcm/ws/1?token={application.token}"
    ) as ws:
        assert ws.receive_json()["type"] == "online_status"
        ws.send_json({"type": "zigbee_knob_command", "button_id": "d", "command": "up"})
        with pytest.raises(WebSocketDisconnect) as closed:
            ws.receive_json()
        assert closed.value.code == 1008
    cm.set_zigbee_knob_command.assert_not_called()


def test_virtual_knob_is_excluded_from_mesh_routing():
    data = {222: {"a": [(111, "a")]}, KNOB_SWITCH_ID: {"a": [(111, "b")]}}
    assert mesh_switch_config(data) == {222: data[222]}
    assert KNOB_SWITCH_ID in data
    assert mesh_switch_config({str(KNOB_SWITCH_ID): {"a": 0}}) == {}


@pytest.mark.parametrize(
    "topic", ["", "zigbee2mqtt/+", "zigbee2mqtt/#", "zigbee2mqtt/\x00"]
)
def test_topic_must_be_exact(topic):
    with pytest.raises(ValidationError):
        ZigbeeKnobSettings(topic=topic)
