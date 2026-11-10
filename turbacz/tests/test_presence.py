"""mmWave presence sensors: telemetry, registration, RCM commands and routing types."""

import json
from threading import RLock
from time import time
from unittest.mock import AsyncMock, MagicMock

import pytest
from fastapi import HTTPException
from starlette.websockets import WebSocketDisconnect

from turbacz.authorization import command_capability
from turbacz.ha.registry import build_registry
from turbacz.mqtt_validation import validate_payload
from turbacz.presence import command_payload, report_config
from turbacz.security import RateLimiter
from turbacz.settings import config
from turbacz.validation import validate_command

from .test_security import application as application

SENSOR = 444
TRIGGER = [60000, 30000, 3000, 2000, 500, 400, 400, 300, 300, 300, 300, 250, 250, 200, 200, 200]
HOLD = [40000, 20000, 400, 300, 300, 200, 200, 150, 150, 100, 100, 100, 100, 100, 100, 100]


def heartbeat(**changes):
    return {
        "deviceId": SENSOR, "parentId": 111, "type": "presence", "firmware": 1, "rssi": -50,
        "uptime": 10, "clicks": 0, "freeHeap": 100000, "presence": 1, "distance": 140, "radar": 1,
        **changes,
    }


def config_report(**changes):
    return {
        "deviceId": SENSOR, "type": "presence", "report": "config", "fw": "v1.5.9",
        "min": 0, "max": 12, "delay": 30, "led": 0, "trigger": TRIGGER, "hold": HOLD, **changes,
    }


def live_report(**changes):
    return {
        "deviceId": SENSOR, "type": "presence", "report": "live", "presence": 1,
        "distance": 210, "energy": list(range(16)), **changes,
    }


def settings(**changes):
    return {"min": 1, "max": 8, "delay": 45, "trigger": TRIGGER, "hold": HOLD, "led": True, **changes}


@pytest.mark.parametrize("payload", [
    heartbeat(), heartbeat(presence=0, radar=0, distance=0), config_report(), config_report(fw=""),
    config_report(trigger=[2**32 - 1] * 16), live_report(), live_report(energy=[65535] * 16, calibrating=30),
])
def test_valid_presence_telemetry(payload):
    validate_payload("/switch/state/root", json.dumps(payload))


@pytest.mark.parametrize("payload", [
    heartbeat(presence=2), heartbeat(distance=-1), heartbeat(radar=True), heartbeat(type="radar"),
    config_report(min=16), config_report(max=-1), config_report(delay=65536), config_report(led=2),
    config_report(trigger=TRIGGER[:15]), config_report(hold=HOLD + [1]), config_report(trigger=[2**32] * 16),
    config_report(hold="100"), config_report(fw="v1\n"), config_report(fw="x" * 16),
    config_report(type="switch"), config_report(report="debug"),
    live_report(energy=[65536] * 16), live_report(calibrating=121), live_report(presence=True),
    {key: value for key, value in live_report().items() if key != "distance"},
])
def test_invalid_presence_telemetry_is_rejected(payload):
    with pytest.raises((ValueError, TypeError, KeyError)):
        validate_payload("/switch/state/root", json.dumps(payload))


@pytest.fixture
def broker_env(application, monkeypatch):
    from turbacz import broker

    manager = MagicMock()
    manager.get_relays.return_value = {}
    manager.get_switches.return_value = {}
    manager.get_presence_sensors.return_value = {}
    manager.get_all_buttons.return_value = {SENSOR: {"a": 2, "b": 3, "c": 4}}
    state = MagicMock()
    client = MagicMock()
    monkeypatch.setattr(broker, "connection_manager", manager)
    monkeypatch.setattr(broker, "state_manager", state)
    monkeypatch.setattr(broker, "ha_bridge", MagicMock())
    monkeypatch.setattr(broker, "record_node_metrics", MagicMock())
    monkeypatch.setattr(broker.ws_manager, "broadcast", AsyncMock())
    monkeypatch.setattr(broker.mqtt, "client", client)
    monkeypatch.setattr(broker, "_config_requests", RateLimiter())
    return broker, manager, state, client


async def test_new_sensor_registers_its_inputs_and_pushes_routing_types(broker_env):
    broker, manager, state, client = broker_env
    await broker.handle_root_state(json.dumps(heartbeat()))
    manager.add_switch.assert_not_called()
    name = manager.add_presence_sensor.call_args.args[1]
    manager.add_presence_sensor.assert_called_once_with(SENSOR, name)
    published = [call.args for call in client.publish.call_args_list]
    types = json.dumps({"type": "button_types", "data": {SENSOR: {"a": 2, "b": 3, "c": 4}}})
    # Types reach the root before the sensor's first event can be routed.
    assert published.index(("/switch/cmd/root", types)) < published.index((f"/switch/cmd/{SENSOR}", "P"))
    broker.ws_manager.broadcast.assert_any_await({"type": "update"}, "/rcm/ws/")
    state.update_presence.assert_called_once_with(SENSOR, presence=1, distance=140, radar=1)
    state.mark_switch_online.assert_called_once()
    state.set_firmware_version.assert_called_once_with(SENSOR, "switch", 1)
    assert broker.record_node_metrics.call_args.args[0]["name"] == name


async def test_known_sensor_keeps_its_name_and_old_switch_records_are_converted(broker_env):
    broker, manager, _state, _client = broker_env
    manager.get_switches.return_value = {SENSOR: ("Hall", 3)}
    manager.get_presence_sensors.return_value = {SENSOR: None}
    await broker.handle_root_state(json.dumps(heartbeat()))
    manager.add_presence_sensor.assert_not_called()
    assert broker.record_node_metrics.call_args.args[0]["name"] == "Hall"
    # First registered as an ordinary switch (e.g. provisioned before its role).
    manager.get_switches.return_value = {SENSOR: ("Hall", 7)}
    manager.get_presence_sensors.return_value = {}
    await broker.handle_root_state(json.dumps(heartbeat()))
    manager.add_presence_sensor.assert_called_once_with(SENSOR, "Hall")


async def test_config_report_is_cached_and_sent_to_rcm(broker_env):
    broker, manager, state, client = broker_env
    manager.get_switches.return_value = {SENSOR: ("Hall", 3)}
    manager.get_presence_sensors.return_value = {SENSOR: None}
    await broker.handle_root_state(json.dumps(config_report(led=1)))
    expected = {"min": 0, "max": 12, "delay": 30, "trigger": TRIGGER, "hold": HOLD, "led": True, "fw": "v1.5.9"}
    manager.set_presence_config.assert_called_once_with(SENSOR, expected)
    broker.ws_manager.broadcast.assert_awaited_once_with(
        {"type": "presence_config", "switch_id": SENSOR, "config": expected}, "/rcm/ws/"
    )
    # A report is not a heartbeat.
    state.mark_switch_online.assert_not_called()
    client.publish.assert_not_called()


async def test_sensors_are_asked_for_settings_turbacz_missed(broker_env, monkeypatch):
    broker, manager, _state, client = broker_env
    now = [0]
    monkeypatch.setattr(broker, "_config_requests", RateLimiter(clock=lambda: now[0]))
    manager.get_switches.return_value = {SENSOR: ("Hall", 3)}
    manager.get_presence_sensors.return_value = {SENSOR: report_config(config_report())}

    def reads():
        read = (f"/switch/cmd/{SENSOR}", '{"radar":"read"}')
        return [call.args for call in client.publish.call_args_list].count(read)

    await broker.handle_root_state(json.dumps(heartbeat()))
    assert reads() == 0
    manager.get_presence_sensors.return_value = {SENSOR: None}
    # A sensor sets up a silent radar again by itself and then reports.
    await broker.handle_root_state(json.dumps(heartbeat(radar=0)))
    assert reads() == 0
    for _ in range(3):
        await broker.handle_root_state(json.dumps(heartbeat()))
    assert reads() == 1
    now[0] = 300  # still unanswered five minutes later
    await broker.handle_root_state(json.dumps(heartbeat()))
    assert reads() == 2


async def test_live_report_reaches_rcm_without_database_writes(broker_env):
    broker, manager, state, _client = broker_env
    await broker.handle_root_state(json.dumps(live_report(calibrating=12)))
    broker.ws_manager.broadcast.assert_awaited_once_with(
        {"type": "presence_live", "switch_id": SENSOR, "presence": 1, "distance": 210,
         "energy": list(range(16)), "calibrating": 12},
        "/rcm/ws/",
    )
    state.update_presence.assert_called_once_with(SENSOR, presence=1, distance=210, radar=1)
    manager.add_presence_sensor.assert_not_called()
    manager.set_presence_config.assert_not_called()


async def test_presence_events_update_occupancy_once_per_change(application, monkeypatch):
    from turbacz import broker
    from turbacz.state_manager import StateManager

    state = StateManager()
    state.set_device_role(SENSOR, {"type": "presence"})
    state.update_presence(SENSOR, presence=0, distance=120, radar=1)
    broadcast = AsyncMock()
    monkeypatch.setattr(broker, "state_manager", state)
    monkeypatch.setattr(broker.ws_manager, "broadcast", broadcast)
    monkeypatch.setattr(broker.ha_bridge, "on_button_event", AsyncMock())
    await broker.handle_switch_state("a1", f"/switch/state/{SENSOR}")
    broadcast.assert_any_await(
        {"type": "presence_state", "switch_id": SENSOR, "presence": 1, "distance": 120, "radar": 1}, "/rcm/ws/"
    )
    # Arrival and departure inputs repeat the change; only 'a' updates occupancy.
    broadcast.reset_mock()
    await broker.handle_switch_state("c0", f"/switch/state/{SENSOR}")
    assert broadcast.await_count == 1
    assert state.get_presence(SENSOR)["presence"] == 1


def test_presence_commands_are_validated(cm):
    cm.presence_sensors = {SENSOR: None}
    for command in [
        {"action": "apply", "config": settings()},
        {"action": "apply", "config": settings(min=12, max=12, delay=0, led=False)},
        {"action": "apply", "config": settings(trigger=[2**32 - 1] * 16, hold=[0] * 16)},
        {"action": "read"},
        {"action": "factory"},
        {"action": "calibrate", "seconds": 5},
        {"action": "calibrate", "seconds": 60},
        {"action": "live", "seconds": 0},
        {"action": "live", "seconds": 120},
    ]:
        validate_command("/rcm/ws/1", {"type": "presence_command", "switch_id": SENSOR, **command}, cm)


@pytest.mark.parametrize("command", [
    {"switch_id": 222, "action": "read"},  # an ordinary switch
    {"action": "reboot"},
    {"action": "apply"},
    {"action": "apply", "config": settings(min=9, max=8)},
    {"action": "apply", "config": settings(max=16)},
    {"action": "apply", "config": settings(delay=65536)},
    {"action": "apply", "config": settings(trigger=TRIGGER[:15])},
    {"action": "apply", "config": settings(hold=[-1] * 16)},
    {"action": "apply", "config": settings(hold=[2**32] * 16)},
    {"action": "apply", "config": settings(trigger=[1.5] * 16)},
    {"action": "apply", "config": settings(led=1)},
    {"action": "apply", "config": {**settings(), "fw": "v1.5.9"}},
    {"action": "calibrate", "seconds": 4},
    {"action": "calibrate", "seconds": 61},
    {"action": "live"},
    {"action": "live", "seconds": 121},
])
def test_invalid_presence_commands_are_rejected(cm, command):
    cm.presence_sensors = {SENSOR: None}
    with pytest.raises((ValueError, TypeError)):
        validate_command("/rcm/ws/1", {"type": "presence_command", "switch_id": SENSOR, **command}, cm)


def test_firmware_commands_fit_one_mesh_frame():
    largest = command_payload({"action": "apply", "config": settings(
        min=15, max=15, delay=65535, trigger=[2**32 - 1] * 16, hold=[2**32 - 1] * 16)})
    assert len(largest) < 512  # MESH_MSG_DATA_SIZE, including the terminator
    assert json.loads(command_payload({"action": "apply", "config": settings()})) == {
        "radar": "apply", "min": 1, "max": 8, "delay": 45, "trigger": TRIGGER, "hold": HOLD, "led": 1}
    assert command_payload({"action": "live", "seconds": 60}) == '{"radar":"live","seconds":60}'
    assert command_payload({"action": "factory"}) == '{"radar":"factory"}'
    assert report_config(config_report())["led"] is False


@pytest.mark.parametrize("action,capability", [
    ("apply", "admin"), ("factory", "admin"), ("calibrate", "admin"), ("read", "operator"), ("live", "operator"),
])
def test_presence_command_capabilities(action, capability):
    assert command_capability("/rcm/ws/1", {"type": "presence_command", "action": action}) == capability


def test_button_types_accept_presence_routing(cm):
    cm.buttons[SENSOR] = {"a": 2, "b": 3, "c": 4}
    validate_command("/rcm/ws/1", {"type": "button_types", "data": {str(SENSOR): {"a": 2, "b": 3, "c": 4}}}, cm)
    with pytest.raises(ValueError):
        validate_command("/rcm/ws/1", {"type": "button_types", "data": {str(SENSOR): {"a": 5}}}, cm)


def test_rcm_presence_commands_reach_only_online_sensors(application, monkeypatch):
    a = application
    a.cm.presence_sensors = {SENSOR: None}
    state = a.main.state_manager
    monkeypatch.setattr(state, "_online_switches", {})
    with a.http.websocket_connect(f"/rcm/ws/1?token={a.token}") as ws:
        assert "presence_states" in ws.receive_json()
        ws.send_json({"type": "presence_command", "switch_id": SENSOR, "action": "read"})
        assert ws.receive_json() == {"type": "error", "message": "The presence sensor is offline"}
        state.mark_switch_online(SENSOR, int(time()))
        a.mqtt.drain()
        ws.send_json({"type": "presence_command", "switch_id": SENSOR, "action": "apply", "config": settings()})
        ws.send_json({"type": "ping"})
        assert ws.receive_json()["type"] == "pong"
        assert a.mqtt.drain() == [
            (f"/switch/cmd/{SENSOR}", command_payload({"action": "apply", "config": settings()}), False)
        ]


def test_operators_can_watch_but_not_change_presence_settings(application, monkeypatch):
    a = application
    a.cm.presence_sensors = {SENSOR: None}
    monkeypatch.setitem(config.roles, "test@example.com", {"operator"})
    monkeypatch.setattr(a.main.state_manager, "_online_switches", {SENSOR: int(time())})
    with a.http.websocket_connect(f"/rcm/ws/1?token={a.token}") as ws:
        assert ws.receive_json()["type"] == "online_status"
        a.mqtt.drain()
        ws.send_json({"type": "presence_command", "switch_id": SENSOR, "action": "live", "seconds": 60})
        ws.send_json({"type": "ping"})
        assert ws.receive_json()["type"] == "pong"
        assert a.mqtt.drain() == [(f"/switch/cmd/{SENSOR}", '{"radar":"live","seconds":60}', False)]
        ws.send_json({"type": "presence_command", "switch_id": SENSOR, "action": "factory"})
        with pytest.raises(WebSocketDisconnect) as closed:
            ws.receive_json()
        assert closed.value.code == 1008
    assert not a.mqtt.sent


def test_presence_sensors_endpoint_requires_authentication(application):
    a = application
    a.cm.presence_sensors = {SENSOR: report_config(config_report())}
    assert a.http.get("/lights/get_presence_sensors").status_code == 401
    response = a.http.get("/lights/get_presence_sensors", headers={"Authorization": f"Bearer {a.token}"})
    assert response.json() == {str(SENSOR): report_config(config_report())}


def test_presence_inputs_report_presence_and_clear_events(cm):
    cm.switches[SENSOR] = ("Hall", 3)
    cm.buttons[SENSOR] = {"a": 2, "b": 3, "c": 4}
    registry = build_registry(cm, "domator", "homeassistant")
    for button in "abc":
        assert registry.buttons[(SENSOR, button)] == ["press", "release"]
    assert registry.buttons[(222, "a")] == ["press"]


def test_presence_sensor_inputs_cannot_be_renumbered(application):
    from turbacz.connection_manager import ConnectionManager

    manager = ConnectionManager.__new__(ConnectionManager)
    manager._db_lock = RLock()
    manager._registry_cache = {}
    manager.conn = MagicMock()
    manager.get_switches = lambda: {SENSOR: ("Hall", 3)}
    manager.get_presence_sensors = lambda: {SENSOR: None}
    with pytest.raises(HTTPException) as error:
        manager.rename_switch(SENSOR, "Hall", 4)
    assert error.value.status_code == 400
    manager.rename_switch(SENSOR, "Hallway", 3)
    manager.conn.commit.assert_called()
