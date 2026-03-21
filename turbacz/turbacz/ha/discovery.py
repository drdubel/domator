"""Build Home Assistant MQTT Discovery payloads."""

from turbacz.ha import topics as T
from turbacz.ha.models import CapabilityType, HACapability


def _device_block(home_id: str, home_name: str, area_name: str | None = None) -> dict:
    block: dict = {
        "ids": [f"domator_{home_id}"],
        "name": home_name,
        "mf": "drdubel",
        "mdl": "Domator",
    }
    if area_name:
        block["suggested_area"] = area_name
    return block


def _availability(home_id: str) -> dict:
    return {
        "avty_t": T.availability_topic(home_id),
        "pl_avail": "online",
        "pl_not_avail": "offline",
    }


def build_light_payload(
    cap: HACapability,
    home_id: str,
    home_name: str,
    area_id: str,
    area_name: str | None = None,
) -> dict:
    uid = T.unique_id(home_id, area_id, cap.device_id, cap.capability_type.value)
    return {
        "name": cap.name,
        "uniq_id": uid,
        "cmd_t": T.command_topic(home_id, area_id, cap.device_id, cap.capability_type.value),
        "stat_t": T.state_topic(home_id, area_id, cap.device_id, cap.capability_type.value),
        "pl_on": "ON",
        "pl_off": "OFF",
        **_availability(home_id),
        "dev": _device_block(home_id, home_name, area_name),
    }


def build_cover_payload(
    cap: HACapability,
    home_id: str,
    home_name: str,
    area_id: str,
    area_name: str | None = None,
) -> dict:
    uid = T.unique_id(home_id, area_id, cap.device_id, cap.capability_type.value)
    return {
        "name": cap.name,
        "uniq_id": uid,
        "cmd_t": T.command_topic(home_id, area_id, cap.device_id, cap.capability_type.value),
        "stat_t": T.state_topic(home_id, area_id, cap.device_id, cap.capability_type.value),
        "set_pos_t": T.cover_set_position_topic(home_id, area_id, cap.device_id),
        "pos_t": T.cover_position_topic(home_id, area_id, cap.device_id),
        "pl_open": "OPEN",
        "pl_cls": "CLOSE",
        "pl_stop": "STOP",
        "stat_open": "open",
        "stat_closed": "closed",
        "stat_opening": "opening",
        "stat_closing": "closing",
        **_availability(home_id),
        "dev": _device_block(home_id, home_name, area_name),
    }


def build_climate_payload(
    cap: HACapability,
    home_id: str,
    home_name: str,
    area_id: str,
    area_name: str | None = None,
) -> dict:
    uid = T.unique_id(home_id, area_id, cap.device_id, cap.capability_type.value)
    return {
        "name": cap.name,
        "uniq_id": uid,
        "mode_cmd_t": T.climate_mode_command_topic(home_id, area_id, cap.device_id),
        "mode_stat_t": T.climate_mode_state_topic(home_id, area_id, cap.device_id),
        "temp_cmd_t": T.climate_target_temp_command_topic(home_id, area_id, cap.device_id),
        "temp_stat_t": T.climate_target_temp_state_topic(home_id, area_id, cap.device_id),
        "curr_temp_t": T.climate_current_temp_topic(home_id, area_id, cap.device_id),
        "modes": ["off", "heat", "auto"],
        "min_temp": 7,
        "max_temp": 30,
        "temp_step": 0.5,
        **_availability(home_id),
        "dev": _device_block(home_id, home_name, area_name),
    }


_BUILDERS = {
    CapabilityType.light: build_light_payload,
    CapabilityType.cover: build_cover_payload,
    CapabilityType.climate: build_climate_payload,
}


def build_discovery_payload(
    cap: HACapability,
    home_id: str,
    home_name: str,
    area_id: str,
    area_name: str | None = None,
) -> dict:
    """Dispatch to the correct builder based on capability type."""
    builder = _BUILDERS[cap.capability_type]
    return builder(cap, home_id, home_name, area_id, area_name)
