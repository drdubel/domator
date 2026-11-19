"""The lights page socket: what a page is shown, who may change what, and section edits."""

from time import time
from unittest.mock import AsyncMock, MagicMock, call

import pytest

from turbacz.authorization import command_capability
from turbacz.settings import config
from turbacz.validation import validate_command

from .test_security import application as application


@pytest.fixture
def page(application, monkeypatch):
    """Relay 111 reports Ceiling on, Counter off and a running blind."""
    state = application.main.state_manager
    monkeypatch.setattr(state, "_states", {111: {"a": 1, "b": 0, "c": 1}})
    monkeypatch.setattr(state, "_online_relays", {111: int(time())})
    monkeypatch.setattr(application.main.ha_bridge, "schedule_resync", MagicMock())
    return application


def connect(page):
    return page.http.websocket_connect(f"/lights/ws/1?token={page.token}")


def until_pong(ws):
    """Everything the server sends before it answers a ping."""
    ws.send_json({"type": "ping"})
    messages = []
    while (message := ws.receive_json()) != {"type": "pong"}:
        messages.append(message)
    return messages


def test_page_gets_its_lights_permissions_and_live_state_without_blind_outputs(page):
    with connect(page) as ws:
        configuration = ws.receive_json()
        assert configuration["type"] == "configuration"
        # c and d drive the kitchen blind; e still has its default name.
        assert configuration["named_outputs"] == {"111": {"a": ["Ceiling", 2, 0, 0], "b": ["Counter", 2, 1, 0]}}
        assert configuration["sections"] == {"1": "Default", "2": "Kitchen"}
        assert configuration["section_order"] == [1, 2]
        assert configuration["default_section"] == 1
        assert configuration["permissions"] == {"control": True, "edit": True}
        assert configuration["online_relays"] == [111]
        assert configuration["states"] == {"111": {"a": 1, "b": 0, "c": 1}}
        # Clients that predate the states in the configuration still get them one by one.
        assert {"type": "light_state", "relay_id": 111, "output_id": "a", "state": 1} in until_pong(ws)


def test_operator_switches_lights_and_hears_that_the_layout_is_for_admins(page, monkeypatch):
    monkeypatch.setattr(config, "roles", {"test@example.com": {"operator"}})
    with connect(page) as ws:
        assert ws.receive_json()["permissions"] == {"control": True, "edit": False}
        until_pong(ws)
        page.mqtt.drain()
        ws.send_json({
            "type": "layout_update", "relay_id": 111, "output_id": "b", "section": 1,
            "positions": [{"relay_id": 111, "output_id": "b", "output_idx": 0}],
        })
        error, resync = until_pong(ws)
        assert error == {"type": "error", "message": "Only admins can change the lights layout"}
        # The current layout undoes the move the page already made on screen.
        assert resync["type"] == "configuration"
        assert resync["named_outputs"]["111"]["b"] == ["Counter", 2, 1, 0]
        assert page.cm.outputs[111]["b"] == ("Counter", 2, 1, 0)
        ws.send_json({"relay_id": "111", "output_id": "a", "state": 0})
        assert until_pong(ws) == []
        assert page.mqtt.drain() == [("/relay/cmd/111", "a0", False)]


def test_viewer_is_told_instead_of_disconnected(page, monkeypatch):
    monkeypatch.setattr(config, "roles", {})
    with connect(page) as ws:
        assert ws.receive_json()["permissions"] == {"control": False, "edit": False}
        until_pong(ws)
        page.mqtt.drain()
        for command in ({"relay_id": 111, "output_id": "a", "state": 0}, {"type": "all_off"}):
            ws.send_json(command)
            assert until_pong(ws) == [{"type": "error", "message": "Your account can view the lights but not switch them"}]
    assert not page.mqtt.sent


def test_blind_outputs_are_only_switched_from_the_blinds_page(page, cm):
    with connect(page) as ws:
        until_pong(ws)
        page.mqtt.drain()
        ws.send_json({"relay_id": 111, "output_id": "d", "state": 1})
        assert until_pong(ws) == [{"type": "error", "message": "Blinds are controlled from the Blinds page"}]
    assert not page.mqtt.sent
    # The connection manager still drives any output.
    validate_command("/rcm/ws/1", {"relay_id": 111, "output_id": "d", "state": 1}, cm)


def test_admin_adds_renames_reorders_and_deletes_sections(page):
    with connect(page) as ws:
        until_pong(ws)
        ws.send_json({"type": "add_section", "name": "  Garden "})
        (configuration,) = until_pong(ws)
        assert configuration["sections"]["3"] == "Garden"
        assert configuration["section_order"] == [1, 2, 3]
        # Broadcasts carry the layout only; every page keeps its own permissions.
        assert "permissions" not in configuration and "states" not in configuration

        ws.send_json({"type": "add_section", "name": "kitchen"})
        error, resync = until_pong(ws)
        assert error == {"type": "error", "message": "A section with this name already exists"}
        assert resync["sections"] == configuration["sections"]

        ws.send_json({"type": "rename_section", "section": 3, "name": "Patio"})
        ws.send_json({"type": "reorder_sections", "order": [3, 1, 2]})
        *_, configuration = until_pong(ws)
        assert configuration["sections"]["3"] == "Patio"
        assert configuration["section_order"] == [3, 1, 2]

        ws.send_json({"type": "remove_section", "section": 2})
        (configuration,) = until_pong(ws)
        assert configuration["section_order"] == [3, 1]
        # The kitchen lights move to the default section.
        assert configuration["named_outputs"]["111"]["a"] == ["Ceiling", 1, 0, 0]

        for command, message in [
            ({"type": "remove_section", "section": 1}, "New lights are added to the default section, so it can't be deleted"),
            ({"type": "reorder_sections", "order": [1, 1]}, "The section order must list every section once"),
            ({"type": "rename_section", "section": 9, "name": "Nowhere"}, "Unknown section"),
        ]:
            ws.send_json(command)
            error, _ = until_pong(ws)
            assert error == {"type": "error", "message": message}
    assert page.cm.sections == {3: "Patio", 1: "Default"}
    # Home Assistant devices follow section names and membership.
    assert page.main.ha_bridge.schedule_resync.call_count == 2


def test_all_off_switches_off_every_light_not_known_to_be_off(page):
    page.cm.outputs[111]["f"] = ("Desk", 1, 0, 0)  # never reported
    with connect(page) as ws:
        until_pong(ws)
        page.mqtt.drain()
        ws.send_json({"type": "all_off", "section": 2})
        until_pong(ws)
        assert page.mqtt.drain() == [("/relay/cmd/111", "a0", False)]
        ws.send_json({"type": "all_off"})
        until_pong(ws)
        # Counter is already off, c and d drive a blind and e is not a named light.
        assert page.mqtt.drain() == [("/relay/cmd/111", "a0", False), ("/relay/cmd/111", "f0", False)]


def test_moving_a_light_to_another_section_updates_home_assistant(page):
    with connect(page) as ws:
        until_pong(ws)
        ws.send_json({
            "type": "layout_update", "relay_id": 111, "output_id": "b", "section": 1,
            "positions": [
                {"relay_id": 111, "output_id": "a", "output_idx": 0},
                {"relay_id": 111, "output_id": "b", "output_idx": 0},
            ],
        })
        (configuration,) = until_pong(ws)
    assert configuration["named_outputs"]["111"]["b"] == ["Counter", 1, 0, 0]
    assert page.cm.outputs[111]["b"] == ("Counter", 1, 0, 0)
    page.main.ha_bridge.schedule_resync.assert_called_once()


async def test_lights_pages_hear_when_relays_come_and_go(monkeypatch):
    from turbacz import state_manager as module

    state = module.StateManager()
    broadcast = AsyncMock()
    monkeypatch.setattr(module.ws_manager, "broadcast", broadcast)
    state.mark_relay_online(111, int(time()))
    await state.send_relay_status()
    await state.send_relay_status()
    state.mark_relay_offline(111)
    await state.send_relay_status()
    assert broadcast.await_args_list == [
        call({"type": "relay_status", "online_relays": [111]}, "/lights/ws/"),
        call({"type": "relay_status", "online_relays": []}, "/lights/ws/"),
    ]


@pytest.mark.parametrize(
    "command,capability",
    [
        ({"type": "rename_section"}, "admin"),
        ({"type": "remove_section"}, "admin"),
        ({"type": "reorder_sections"}, "admin"),
        ({"type": "all_off"}, "operator"),
    ],
)
def test_section_edits_need_admin_and_all_off_needs_operator(command, capability):
    assert command_capability("/lights/ws/1", command) == capability


def test_section_names_are_unique_regardless_of_case_and_spacing(cm):
    validate_command("/lights/ws/1", {"type": "rename_section", "section": 2, "name": "KITCHEN"}, cm)
    with pytest.raises(ValueError, match="already exists"):
        validate_command("/lights/ws/1", {"type": "rename_section", "section": 2, "name": " default "}, cm)
    with pytest.raises(ValueError, match="Invalid name"):
        validate_command("/lights/ws/1", {"type": "add_section", "name": "x" * 65}, cm)
    with pytest.raises(ValueError, match="Unknown section"):
        validate_command("/lights/ws/1", {"type": "all_off", "section": 3}, cm)


def test_static_files_are_revalidated_on_every_load(application, tmp_path):
    (tmp_path / "static" / "lights.js").write_text("// lights")
    response = application.http.get("/static/lights.js")
    assert response.headers["cache-control"] == "no-cache"
    unchanged = application.http.get("/static/lights.js", headers={"If-None-Match": response.headers["etag"]})
    assert unchanged.status_code == 304
    assert unchanged.headers["cache-control"] == "no-cache"
