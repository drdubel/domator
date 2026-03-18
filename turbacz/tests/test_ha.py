"""Unit tests for the HA bridge — no MQTT broker or database required."""

import json
from unittest.mock import MagicMock

import pytest

from turbacz.ha.models import (
    CapabilityType,
    HAApplyResult,
    HACapability,
    HADevice,
    HAArea,
    HAHome,
)
from turbacz.ha import topics as T
from turbacz.ha.discovery import build_discovery_payload, build_light_payload, build_cover_payload, build_climate_payload
from turbacz.ha.apply import apply, get_command_topics, build_lights_from_turbacz, _discovery_topic_for_cap


# ── Topic generation ──────────────────────────────────────────────────────────


class TestTopics:
    def test_unique_id_format(self):
        uid = T.unique_id("h1", "a1", "d1", "light")
        assert uid == "domator_h1_a1_d1_light"

    def test_unique_id_is_stable(self):
        """Same inputs always produce same ID."""
        assert T.unique_id("h1", "a1", "d1", "light") == T.unique_id("h1", "a1", "d1", "light")

    def test_discovery_topic(self):
        topic = T.discovery_topic("light", "domator_h1_a1_d1_light")
        assert topic == "homeassistant/light/domator_h1_a1_d1_light/config"

    def test_state_topic(self):
        assert T.state_topic("h", "a", "d", "light") == "domator/h/a/d/light/state"

    def test_command_topic(self):
        assert T.command_topic("h", "a", "d", "cover") == "domator/h/a/d/cover/set"

    def test_availability_topic(self):
        assert T.availability_topic("home1") == "domator/home1/status"

    def test_cover_position_topics(self):
        assert T.cover_position_topic("h", "a", "d") == "domator/h/a/d/cover/position"
        assert T.cover_set_position_topic("h", "a", "d") == "domator/h/a/d/cover/position/set"

    def test_climate_topics(self):
        assert T.climate_mode_state_topic("h", "a", "d") == "domator/h/a/d/climate/mode/state"
        assert T.climate_mode_command_topic("h", "a", "d") == "domator/h/a/d/climate/mode/set"
        assert T.climate_target_temp_state_topic("h", "a", "d") == "domator/h/a/d/climate/target/state"
        assert T.climate_target_temp_command_topic("h", "a", "d") == "domator/h/a/d/climate/target/set"
        assert T.climate_current_temp_topic("h", "a", "d") == "domator/h/a/d/climate/current/state"

    def test_topics_under_domator_namespace(self):
        """All data-plane topics should start with 'domator/'."""
        for fn in (T.state_topic, T.command_topic):
            assert fn("h", "a", "d", "light").startswith("domator/")


# ── Discovery payloads ────────────────────────────────────────────────────────


def _make_cap(cap_type: CapabilityType, name: str = "Test") -> HACapability:
    return HACapability(id="cap1", device_id="dev1", capability_type=cap_type, name=name)


class TestDiscoveryPayloads:
    HOME_ID = "home1"
    HOME_NAME = "My Home"
    AREA_ID = "area1"

    def test_light_payload_keys(self):
        cap = _make_cap(CapabilityType.light, "Living Room Light")
        payload = build_light_payload(cap, self.HOME_ID, self.HOME_NAME, self.AREA_ID)
        assert payload["name"] == "Living Room Light"
        assert "uniq_id" in payload
        assert payload["cmd_t"] == T.command_topic(self.HOME_ID, self.AREA_ID, "dev1", "light")
        assert payload["stat_t"] == T.state_topic(self.HOME_ID, self.AREA_ID, "dev1", "light")
        assert payload["pl_on"] == "ON"
        assert payload["pl_off"] == "OFF"
        assert "avty_t" in payload
        assert payload["dev"]["ids"] == [f"domator_{self.HOME_ID}"]

    def test_cover_payload_keys(self):
        cap = _make_cap(CapabilityType.cover, "Living Room Blinds")
        payload = build_cover_payload(cap, self.HOME_ID, self.HOME_NAME, self.AREA_ID)
        for key in ("cmd_t", "stat_t", "set_pos_t", "pos_t", "pl_open", "pl_cls", "pl_stop"):
            assert key in payload, f"Missing key: {key}"
        assert payload["stat_open"] == "open"
        assert payload["stat_closed"] == "closed"

    def test_climate_payload_keys(self):
        cap = _make_cap(CapabilityType.climate, "Living Room Heating")
        payload = build_climate_payload(cap, self.HOME_ID, self.HOME_NAME, self.AREA_ID)
        for key in ("mode_cmd_t", "mode_stat_t", "temp_cmd_t", "temp_stat_t", "curr_temp_t"):
            assert key in payload, f"Missing key: {key}"
        assert "off" in payload["modes"]
        assert payload["min_temp"] < payload["max_temp"]

    def test_build_discovery_payload_dispatches(self):
        for cap_type in CapabilityType:
            cap = _make_cap(cap_type)
            payload = build_discovery_payload(cap, self.HOME_ID, self.HOME_NAME, self.AREA_ID)
            assert payload["uniq_id"] == T.unique_id(
                self.HOME_ID, self.AREA_ID, "dev1", cap_type.value
            )

    def test_payload_is_json_serialisable(self):
        for cap_type in CapabilityType:
            cap = _make_cap(cap_type)
            payload = build_discovery_payload(cap, self.HOME_ID, self.HOME_NAME, self.AREA_ID)
            json.dumps(payload)  # must not raise

    def test_unique_id_does_not_contain_display_name(self):
        cap = _make_cap(CapabilityType.light, "Fancy Display Name")
        payload = build_light_payload(cap, self.HOME_ID, self.HOME_NAME, self.AREA_ID)
        assert "Fancy" not in payload["uniq_id"]
        assert "Display" not in payload["uniq_id"]


# ── Apply pipeline ────────────────────────────────────────────────────────────


def _mock_cm(sections=None, named_outputs=None):
    """Build a minimal ConnectionManager mock for apply() tests."""
    cm = MagicMock()
    cm.get_sections.return_value = sections if sections is not None else {1: "Living Room"}
    cm.get_named_outputs.return_value = (
        named_outputs if named_outputs is not None
        else {100: {"a": ("Ceiling Light", 1, 0, 0)}}
    )
    return cm


def _mock_db(applied=None):
    """Build a minimal HADatabase mock (applied-topics tracking only)."""
    db = MagicMock()
    db.get_applied_topics.return_value = applied if applied is not None else {}
    return db


class TestApply:
    def test_apply_publishes_discovery_and_availability(self):
        mqtt_client = MagicMock()
        cm = _mock_cm()
        db = _mock_db()

        result = apply(mqtt_client, db, cm)

        assert len(result.published) == 1
        assert len(result.removed) == 0
        assert not result.errors

        # Availability published
        avail_calls = [c for c in mqtt_client.publish.call_args_list if "status" in c.args[0]]
        assert len(avail_calls) == 1
        assert avail_calls[0].args[1] == "online"

        # Discovery retained
        disc_calls = [c for c in mqtt_client.publish.call_args_list if "homeassistant" in c.args[0]]
        assert len(disc_calls) == 1
        assert disc_calls[0].kwargs.get("retain") or disc_calls[0].args[-1]

    def test_apply_records_published_topic_in_db(self):
        mqtt_client = MagicMock()
        cm = _mock_cm()
        db = _mock_db()

        apply(mqtt_client, db, cm)

        db.upsert_applied_topic.assert_called_once()
        topic_arg = db.upsert_applied_topic.call_args.args[0]
        assert "homeassistant/light/" in topic_arg

    def test_apply_clears_removed_entities(self):
        mqtt_client = MagicMock()
        stale_topic = "homeassistant/light/domator_oldhome_oldarea_olddev_light/config"
        cm = _mock_cm()
        db = _mock_db(applied={stale_topic: "old_cap_id"})

        result = apply(mqtt_client, db, cm)

        assert stale_topic in result.removed
        # Empty retained payload clears the entity from HA
        clear_call = next(c for c in mqtt_client.publish.call_args_list if stale_topic in c.args[0])
        assert clear_call.args[1] == ""
        db.delete_applied_topic.assert_called_with(stale_topic)

    def test_apply_idempotent_no_double_remove(self):
        mqtt_client = MagicMock()
        cm = _mock_cm()
        # Topic that matches the current light (relay 100, output a, section 1)
        existing_topic = T.discovery_topic("light", T.unique_id("home", "s1", "r100_a", "light"))
        db = _mock_db(applied={existing_topic: "r100_a_light"})

        result1 = apply(mqtt_client, db, cm)
        result2 = apply(mqtt_client, db, cm)

        # The existing topic belongs to the current output — never removed
        assert len(result1.removed) == 0
        assert len(result2.removed) == 0

    def test_apply_returns_errors_on_mqtt_failure(self):
        mqtt_client = MagicMock()
        mqtt_client.publish.side_effect = RuntimeError("broker down")
        cm = _mock_cm()
        db = _mock_db()

        result = apply(mqtt_client, db, cm)

        assert result.errors

    def test_apply_creates_light_per_named_output(self):
        mqtt_client = MagicMock()
        cm = _mock_cm(
            sections={1: "Living Room", 2: "Bedroom"},
            named_outputs={
                100: {"a": ("Light A", 1, 0, 0), "b": ("Light B", 2, 0, 0)},
                200: {"a": ("Light C", 1, 0, 0)},
            },
        )
        db = _mock_db()

        result = apply(mqtt_client, db, cm)

        assert len(result.published) == 3  # three named outputs → three lights

    def test_apply_skips_unnamed_outputs(self):
        """Outputs with default 'Output X' names must not be published."""
        mqtt_client = MagicMock()
        cm = _mock_cm(
            sections={1: "Room"},
            named_outputs={},  # get_named_outputs already filters unnamed outputs
        )
        db = _mock_db()

        result = apply(mqtt_client, db, cm)

        disc_calls = [c for c in mqtt_client.publish.call_args_list if "homeassistant" in c.args[0]]
        assert disc_calls == []
        assert result.published == []


# ── build_lights_from_turbacz ─────────────────────────────────────────────────


class TestBuildLightsFromTurbacz:
    def test_returns_empty_when_no_named_outputs(self):
        cm = _mock_cm(sections={1: "Room"}, named_outputs={})
        result = build_lights_from_turbacz(cm)
        assert result == []

    def test_named_output_becomes_light_entity(self):
        cm = _mock_cm()
        homes = build_lights_from_turbacz(cm)
        assert len(homes) == 1
        home = homes[0]
        assert home.id == "home"
        assert len(home.areas) == 1
        area = home.areas[0]
        assert area.id == "s1"
        assert len(area.devices) == 1
        device = area.devices[0]
        assert device.id == "r100_a"
        assert len(device.capabilities) == 1
        assert device.capabilities[0].capability_type == CapabilityType.light

    def test_uncategorized_area_for_section_zero(self):
        cm = _mock_cm(
            sections={1: "Room"},
            named_outputs={100: {"a": ("Light", 0, 0, 0)}},  # section_id=0 → not in sections
        )
        homes = build_lights_from_turbacz(cm)
        assert homes[0].areas[0].name == "Uncategorized"

    def test_stable_ids(self):
        """IDs are derived from relay/output identifiers, not display names."""
        cm = _mock_cm()
        homes = build_lights_from_turbacz(cm)
        device = homes[0].areas[0].devices[0]
        assert device.id == "r100_a"
        assert device.capabilities[0].id == "r100_a_light"


# ── get_command_topics ────────────────────────────────────────────────────────


class TestGetCommandTopics:
    def test_one_command_topic_per_named_output(self):
        cm = _mock_cm()  # one named output
        topics = get_command_topics(cm)
        assert len(topics) == 1
        assert topics[0].endswith("/light/set")

    def test_multiple_outputs_yield_multiple_topics(self):
        cm = _mock_cm(
            sections={1: "Room"},
            named_outputs={100: {"a": ("L1", 1, 0, 0), "b": ("L2", 1, 0, 0)}},
        )
        topics = get_command_topics(cm)
        assert len(topics) == 2

    def test_no_topics_when_no_named_outputs(self):
        cm = _mock_cm(sections={1: "Room"}, named_outputs={})
        assert get_command_topics(cm) == []

    def test_topic_contains_stable_device_id(self):
        cm = _mock_cm()
        topics = get_command_topics(cm)
        assert "r100_a" in topics[0]
