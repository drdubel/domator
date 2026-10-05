"""Remote roles retain switch mappings and gateway commands validate before MQTT."""
from unittest.mock import MagicMock, patch

import pytest

from turbacz.validation import validate_command


@pytest.fixture
def state():
    connection = MagicMock()
    cursor = connection.cursor.return_value.__enter__.return_value
    cursor.execute.return_value = cursor
    cursor.fetchall.return_value = []
    cursor.fetchone.return_value = [1]
    with patch("psycopg.connect", return_value=connection):
        from turbacz.state_manager import StateManager
    return StateManager()


def test_gateway_command_requires_known_switch_and_boolean(cm):
    validate_command("/rcm/ws/1", {"type": "gateway_mode", "device_id": 222, "enabled": True}, cm)
    for device, enabled in [(111, True), (999, True), (222, 1), (222, "false")]:
        with pytest.raises(ValueError):
            validate_command("/rcm/ws/1", {"type": "gateway_mode", "device_id": device, "enabled": enabled}, cm)


async def test_remote_heartbeat_does_not_expire_at_powered_switch_timeout(state):
    state.set_device_role(222, {"type": "remote"})
    state.set_device_role(333, {"type": "switch"})
    state.mark_switch_online(222, 1000)
    state.mark_switch_online(333, 1000)
    with patch("turbacz.state_manager.time", return_value=1061):
        await state.check_switches_if_online()
        assert state.is_switch_online(222)
        assert not state.is_switch_online(333)
    with patch("turbacz.state_manager.time", return_value=1151):
        await state.check_switches_if_online()
        assert not state.is_switch_online(222)


def test_gateway_state_tracks_reported_status_and_address(state):
    state.set_device_role(222, {"type": "switch", "gateway": True,
        "gatewayMac": "02:00:00:00:00:01", "radioChannel": 11})
    assert state.get_device_role(222) == {"type": "switch", "gateway": True,
        "gateway_mac": "02:00:00:00:00:01", "channel": 11}
    state.set_device_role(222, {"type": "switch", "gateway": False})
    assert not state.get_device_role(222)["gateway"]
