"""Unit tests for the HA bridge — no MQTT broker or database required."""

import json
from unittest.mock import MagicMock, call, patch

import pytest

from turbacz.ha.models import (
    CapabilityType,
    HAApplyResult,
    HACapability,
    HACapabilityCreate,
    HADevice,
    HADeviceCreate,
    HAArea,
    HAAreaCreate,
    HAHome,
    HAHomeCreate,
)
from turbacz.ha import topics as T
from turbacz.ha.discovery import build_discovery_payload, build_light_payload, build_cover_payload, build_climate_payload
from turbacz.ha.apply import apply, get_command_topics, _discovery_topic_for_cap


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


def _build_tree(cap_type: CapabilityType = CapabilityType.light):
    cap = HACapability(id="cap1", device_id="dev1", capability_type=cap_type, name="Test Cap")
    device = HADevice(id="dev1", area_id="area1", name="Test Device", capabilities=[cap])
    area = HAArea(id="area1", home_id="home1", name="Test Area", devices=[device])
    home = HAHome(id="home1", name="Test Home", areas=[area])
    return [home]


def _mock_db(tree=None, applied=None):
    db = MagicMock()
    db.get_full_tree.return_value = tree if tree is not None else _build_tree()
    db.get_applied_topics.return_value = applied if applied is not None else {}
    return db


class TestApply:
    def test_apply_publishes_discovery_and_availability(self):
        mqtt_client = MagicMock()
        db = _mock_db()

        result = apply(mqtt_client, db)

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
        db = _mock_db()

        apply(mqtt_client, db)

        db.upsert_applied_topic.assert_called_once()
        topic_arg = db.upsert_applied_topic.call_args.args[0]
        assert "homeassistant/light/" in topic_arg

    def test_apply_clears_removed_entities(self):
        mqtt_client = MagicMock()
        stale_topic = "homeassistant/light/domator_oldhome_oldarea_olddev_light/config"
        db = _mock_db(applied={stale_topic: "old_cap_id"})

        result = apply(mqtt_client, db)

        assert stale_topic in result.removed
        # Empty retained payload
        clear_call = next(c for c in mqtt_client.publish.call_args_list if stale_topic in c.args[0])
        assert clear_call.args[1] == ""
        db.delete_applied_topic.assert_called_with(stale_topic)

    def test_apply_idempotent_no_double_publish(self):
        mqtt_client = MagicMock()
        cap = HACapability(id="cap1", device_id="dev1", capability_type=CapabilityType.light, name="Test")
        device = HADevice(id="dev1", area_id="area1", name="Dev", capabilities=[cap])
        area = HAArea(id="area1", home_id="home1", name="Area", devices=[device])
        home = HAHome(id="home1", name="Home", areas=[area])

        existing_topic = T.discovery_topic(
            "light", T.unique_id("home1", "area1", "dev1", "light")
        )
        db = _mock_db(tree=[home], applied={existing_topic: "cap1"})

        result1 = apply(mqtt_client, db)
        result2 = apply(mqtt_client, db)

        # Both runs publish the same topic (upsert) — no duplicates in removed
        assert len(result1.removed) == 0
        assert len(result2.removed) == 0

    def test_apply_returns_errors_on_mqtt_failure(self):
        mqtt_client = MagicMock()
        mqtt_client.publish.side_effect = RuntimeError("broker down")
        db = _mock_db()

        result = apply(mqtt_client, db)

        assert result.errors

    def test_apply_all_capability_types(self):
        mqtt_client = MagicMock()
        caps = [
            HACapability(id=f"cap{i}", device_id="dev1", capability_type=ct, name=ct.value)
            for i, ct in enumerate(CapabilityType)
        ]
        device = HADevice(id="dev1", area_id="area1", name="Dev", capabilities=caps)
        area = HAArea(id="area1", home_id="home1", name="Area", devices=[device])
        home = HAHome(id="home1", name="Home", areas=[area])
        db = _mock_db(tree=[home])

        result = apply(mqtt_client, db)

        assert len(result.published) == len(list(CapabilityType))


# ── get_command_topics ────────────────────────────────────────────────────────


class TestGetCommandTopics:
    def test_light_has_one_command_topic(self):
        db = _mock_db(_build_tree(CapabilityType.light))
        topics = get_command_topics(db)
        assert len(topics) == 1
        assert topics[0].endswith("/set")

    def test_cover_has_two_command_topics(self):
        db = _mock_db(_build_tree(CapabilityType.cover))
        topics = get_command_topics(db)
        assert len(topics) == 2
        assert any("position/set" in t for t in topics)

    def test_climate_has_three_command_topics(self):
        db = _mock_db(_build_tree(CapabilityType.climate))
        topics = get_command_topics(db)
        assert len(topics) == 3
        assert any("mode/set" in t for t in topics)
        assert any("target/set" in t for t in topics)
