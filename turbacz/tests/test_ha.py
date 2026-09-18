"""Tests for the Home Assistant MQTT Discovery bridge.

Everything here runs against the fakes in conftest.py -- no database, no
broker.
"""

import json

import pytest

from turbacz.ha import topics as T
from turbacz.ha.registry import build_registry
from turbacz.mqtt_client import publish_blind_action

from .conftest import BASE, PREFIX, FakeClient

LIGHT_A = f"{PREFIX}/light/domator_light_111_a/config"
LIGHT_B = f"{PREFIX}/light/domator_light_111_b/config"
COVER_C = f"{PREFIX}/cover/domator_cover_111_c/config"


def registry(cm):
    return build_registry(cm, BASE, PREFIX)


# -- topics -------------------------------------------------------------------


def test_topics_key_on_identity_not_section():
    """Moving a light between rooms must not change where its state lives."""
    assert T.state_topic(BASE, T.LIGHT, T.output_key(111, "a")) == "domator/light/111_a/state"
    assert T.command_topic(BASE, T.LIGHT, T.output_key(111, "a")) == "domator/light/111_a/set"
    assert T.unique_id(T.LIGHT, T.output_key(111, "a")) == "domator_light_111_a"
    assert (
        T.discovery_topic(PREFIX, T.LIGHT, T.output_key(111, "a"))
        == "homeassistant/light/domator_light_111_a/config"
    )


def test_bridge_topics_do_not_collide_with_mesh_topics():
    """The mesh protocol lives under a leading slash; ours must not."""
    for topic in (
        T.bridge_status_topic(BASE),
        T.relay_availability_topic(BASE, 111),
        T.state_topic(BASE, T.LIGHT, "111_a"),
        T.heating_target_command_topic(BASE),
    ):
        assert not topic.startswith("/")


# -- registry -----------------------------------------------------------------


def test_blind_pair_legs_are_not_lights(cm):
    """A blind must appear once, as a cover -- not also as two stray lights."""
    reg = registry(cm)
    assert sorted(reg.lights.values()) == ["111_a", "111_b"]
    assert LIGHT_A in reg.entities and LIGHT_B in reg.entities
    assert f"{PREFIX}/light/domator_light_111_c/config" not in reg.entities
    assert f"{PREFIX}/light/domator_light_111_d/config" not in reg.entities


def test_unnamed_outputs_are_not_exposed(cm):
    """Outputs still called "Output N" are unconfigured and stay invisible."""
    reg = registry(cm)
    assert f"{PREFIX}/light/domator_light_111_e/config" not in reg.entities


def test_lights_group_into_section_devices(cm):
    reg = registry(cm)
    dev = reg.entities[LIGHT_A].payload["dev"]
    assert dev["ids"] == ["domator_section_2"]
    assert dev["name"] == "Kitchen"
    assert dev["sa"] == "Kitchen"


def test_outputs_in_an_unknown_section_fall_back_to_uncategorized(cm):
    cm.outputs[111]["a"] = ("Ceiling", 99, 0, 0)
    dev = registry(cm).entities[LIGHT_A].payload["dev"]
    assert dev["ids"] == ["domator_section_0"]
    assert dev["name"] == "Uncategorized"


def test_cover_takes_its_section_from_the_power_output(cm):
    dev = registry(cm).entities[COVER_C].payload["dev"]
    assert dev["name"] == "Kitchen"


def test_cover_advertises_no_position(cm):
    """The hardware has no encoder, so HA must not be told there is a position."""
    payload = registry(cm).entities[COVER_C].payload
    assert "pos_t" not in payload
    assert "set_pos_t" not in payload
    assert payload["name"] == "Kitchen blind"


def test_blind_pair_without_a_custom_name_falls_back_to_the_output_name(cm):
    cm.blind_pair_names = {}
    assert registry(cm).entities[COVER_C].payload["name"] == "Blind power"


def test_button_event_types_follow_button_type(cm):
    reg = registry(cm)
    toggle = reg.entities[f"{PREFIX}/event/domator_event_222_a/config"].payload
    stateful = reg.entities[f"{PREFIX}/event/domator_event_222_b/config"].payload
    assert toggle["evt_typ"] == ["press"]
    assert stateful["evt_typ"] == ["press", "release"]
    assert stateful["dev"]["ids"] == ["domator_switch_222"]


def test_lights_and_covers_depend_on_their_board_being_online(cm):
    payload = registry(cm).entities[LIGHT_A].payload
    assert payload["avty"] == [
        {"t": "domator/status"},
        {"t": "domator/availability/relay_111"},
    ]
    assert payload["avty_mode"] == "all"


def test_water_heater_overrides_the_ha_default_minimum(cm):
    """HA defaults min_temp to 43.3 C, above a normal mixing setpoint."""
    payload = registry(cm).entities[f"{PREFIX}/water_heater/domator_water_heater_heating/config"].payload
    assert payload["min_temp"] == 20
    assert payload["modes"] == ["performance"]
    # The firmware has no mode concept, so HA must not offer to change it.
    assert "mode_cmd_t" not in payload


def test_pid_sensors_are_diagnostic_and_read_only(cm):
    reg = registry(cm)
    kp = reg.entities[f"{PREFIX}/sensor/domator_sensor_heating_kp/config"].payload
    cold = reg.entities[f"{PREFIX}/sensor/domator_sensor_heating_cold/config"].payload
    assert kp["ent_cat"] == "diagnostic"
    assert "cmd_t" not in kp
    assert cold.get("ent_cat") is None
    assert cold["dev_cla"] == "temperature"


# -- apply --------------------------------------------------------------------


@pytest.mark.asyncio
async def test_apply_publishes_retained_discovery(bridge, client):
    await bridge.apply()
    published = {t: (p, r) for t, p, r in client.sent}
    assert published[LIGHT_A][1] is True
    assert json.loads(published[LIGHT_A][0])["uniq_id"] == "domator_light_111_a"
    assert published["domator/status"] == ("online", True)


@pytest.mark.asyncio
async def test_apply_is_idempotent(bridge, client):
    await bridge.apply()
    client.drain()
    await bridge.apply()
    assert [t for t in client.topics() if t.startswith(PREFIX)] == []


@pytest.mark.asyncio
async def test_renaming_keeps_the_unique_id_and_state_topic(bridge, client, cm):
    await bridge.apply()
    before = bridge._registry.entities[LIGHT_A]
    client.drain()

    cm.outputs[111]["a"] = ("Ceiling lamp", 2, 0, 0)
    await bridge.apply()

    after = bridge._registry.entities[LIGHT_A]
    assert after.payload["name"] == "Ceiling lamp"
    assert after.uid == before.uid
    assert after.payload["stat_t"] == before.payload["stat_t"]


@pytest.mark.asyncio
async def test_removed_entities_are_cleared_with_an_empty_retained_payload(bridge, client, cm):
    await bridge.apply()
    client.drain()

    cm.outputs[111]["b"] = ("Output 2", 2, 1, 0)
    await bridge.apply()

    assert (LIGHT_B, "", True) in client.sent
    assert LIGHT_B not in cm.applied


@pytest.mark.asyncio
async def test_applied_topics_are_recorded_for_cleanup_after_a_restart(bridge, cm):
    await bridge.apply()
    assert cm.applied[LIGHT_A] == "domator_light_111_a"

    # A fresh bridge (as after a restart) still knows to clear a stale entity.
    from turbacz.ha.bridge import HABridge

    cm.outputs[111]["a"] = ("Output 1", 2, 0, 0)
    restarted = HABridge()
    restarted._cm = cm
    restarted._client = client = FakeClient()
    await restarted.apply()
    assert (LIGHT_A, "", True) in client.sent


# -- state --------------------------------------------------------------------


@pytest.mark.asyncio
async def test_relay_state_becomes_light_state(bridge, client):
    await bridge.apply()
    client.drain()

    await bridge.on_relay_state(111, "a", 1)
    assert client.drain() == [("domator/light/111_a/state", "ON", True)]

    await bridge.on_relay_state(111, "a", 0)
    assert client.drain() == [("domator/light/111_a/state", "OFF", True)]


@pytest.mark.asyncio
async def test_unchanged_state_is_not_republished(bridge, client):
    await bridge.apply()
    client.drain()
    await bridge.on_relay_state(111, "a", 1)
    client.drain()
    await bridge.on_relay_state(111, "a", 1)
    assert client.sent == []


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "direction, moving, stopped",
    [(0, "opening", "open"), (1, "closing", "closed")],
)
async def test_cover_state_is_inferred_from_the_two_legs(bridge, client, direction, moving, stopped):
    await bridge.apply()
    client.drain()

    await bridge.on_relay_state(111, "d", direction)
    await bridge.on_relay_state(111, "c", 1)
    assert ("domator/cover/111_c/state", moving, True) in client.drain()

    await bridge.on_relay_state(111, "c", 0)
    assert ("domator/cover/111_c/state", stopped, True) in client.drain()


@pytest.mark.asyncio
async def test_cover_reports_nothing_until_it_has_moved(bridge, client):
    """Stopped with no movement history is genuinely unknown, not 'closed'."""
    await bridge.apply()
    client.drain()
    await bridge.on_relay_state(111, "c", 0)
    assert client.sent == []


@pytest.mark.asyncio
async def test_board_availability(bridge, client):
    await bridge.apply()
    client.drain()
    bridge.publish_relay_availability(111, False)
    assert client.drain() == [("domator/availability/relay_111", "offline", True)]
    bridge.publish_relay_availability(111, True)
    assert client.drain() == [("domator/availability/relay_111", "online", True)]


@pytest.mark.asyncio
async def test_heating_metrics_are_mirrored(bridge, client):
    await bridge.apply()
    client.drain()

    await bridge.on_heating_metrics(
        {"cold": 21.5, "mixed": 38.0, "hot": 55.1, "target": 40.0,
         "integral": 1.2, "pid_output": 0.7, "kp": 2, "ki": 0.1, "kd": 0.05}
    )
    sent = dict((t, p) for t, p, _ in client.sent)
    assert sent["domator/water_heater/heating/current"] == "38.0"
    assert sent["domator/water_heater/heating/target/state"] == "40.0"
    assert sent["domator/sensor/heating_cold/state"] == "21.5"
    assert sent["domator/sensor/heating_kd/state"] == "0.05"


@pytest.mark.asyncio
async def test_button_events_are_not_retained(bridge, client):
    """A retained event would replay on every Home Assistant restart."""
    await bridge.apply()
    client.drain()

    await bridge.on_button_event(222, "a", "press")
    topic, payload, retain = client.drain()[0]
    assert topic == "domator/event/222_a/state"
    assert json.loads(payload) == {"event_type": "press"}
    assert retain is False


@pytest.mark.asyncio
async def test_toggle_buttons_do_not_emit_release(bridge, client):
    await bridge.apply()
    client.drain()
    await bridge.on_button_event(222, "a", "release")
    assert client.sent == []
    await bridge.on_button_event(222, "b", "release")
    assert len(client.sent) == 1


# -- commands -----------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.parametrize("payload, expected", [("ON", "a1"), ("OFF", "a0")])
async def test_light_command_translation(bridge, client, payload, expected):
    await bridge.apply()
    client.drain()
    assert await bridge.handle_command("domator/light/111_a/set", payload) is True
    assert client.drain() == [("/relay/cmd/111", expected, False)]


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "payload, expected",
    [
        ("OPEN", [("/relay/cmd/111", "d0", False), ("/relay/cmd/111", "c1", False)]),
        ("CLOSE", [("/relay/cmd/111", "d1", False), ("/relay/cmd/111", "c1", False)]),
        ("STOP", [("/relay/cmd/111", "c0", False)]),
    ],
)
async def test_cover_command_translation(bridge, client, payload, expected):
    await bridge.apply()
    client.drain()
    assert await bridge.handle_command("domator/cover/111_c/set", payload) is True
    assert client.drain() == expected


@pytest.mark.asyncio
async def test_heating_command_translation(bridge, client):
    await bridge.apply()
    client.drain()
    assert await bridge.handle_command("domator/water_heater/heating/target/set", "42.5") is True
    assert client.drain() == [("/heating/cmd", "t42.5", False)]


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "topic, payload",
    [
        ("domator/water_heater/heating/target/set", "bogus"),
        ("domator/cover/111_c/set", "SIDEWAYS"),
        ("domator/light/999_z/set", "ON"),
        ("domator/cover/999_z/set", "OPEN"),
    ],
)
async def test_bad_commands_are_dropped_not_raised(bridge, client, topic, payload):
    await bridge.apply()
    client.drain()
    await bridge.handle_command(topic, payload)
    assert client.sent == []


@pytest.mark.asyncio
async def test_mesh_topics_are_not_claimed_by_the_bridge(bridge):
    assert await bridge.handle_command("/relay/state/111", "A1") is False


@pytest.mark.asyncio
async def test_bridge_is_inert_when_disabled(bridge, client):
    from turbacz.settings import config

    config.ha.enabled = False
    await bridge.apply()
    await bridge.on_relay_state(111, "a", 1)
    assert await bridge.handle_command("domator/light/111_a/set", "ON") is False
    assert client.sent == []


def test_command_subscriptions_cover_every_writable_domain(bridge):
    assert bridge.command_subscriptions() == [
        "domator/light/+/set",
        "domator/cover/+/set",
        "domator/water_heater/heating/target/set",
    ]


# -- shared command helper ----------------------------------------------------


def test_blind_action_helper_matches_the_websocket_protocol():
    """The /blinds WebSocket and the HA bridge must issue identical commands."""
    client = FakeClient()
    assert publish_blind_action(client, 111, "c", "d", "up") is True
    assert client.drain() == [("/relay/cmd/111", "d0", False), ("/relay/cmd/111", "c1", False)]
    assert publish_blind_action(client, 111, "c", "d", "sideways") is False
    assert client.sent == []
