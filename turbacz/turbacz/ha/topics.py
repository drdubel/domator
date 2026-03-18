"""Deterministic topic and unique-ID naming for HA MQTT Discovery."""

HA_DISCOVERY_PREFIX = "homeassistant"
DOMATOR_BASE = "domator"


def unique_id(home_id: str, area_id: str, device_id: str, capability_type: str) -> str:
    """Return a stable unique_id used by Home Assistant to track the entity."""
    return f"domator_{home_id}_{area_id}_{device_id}_{capability_type}"


def availability_topic(home_id: str) -> str:
    return f"{DOMATOR_BASE}/{home_id}/status"


def discovery_topic(component: str, uid: str) -> str:
    return f"{HA_DISCOVERY_PREFIX}/{component}/{uid}/config"


def state_topic(home_id: str, area_id: str, device_id: str, capability_type: str) -> str:
    return f"{DOMATOR_BASE}/{home_id}/{area_id}/{device_id}/{capability_type}/state"


def command_topic(home_id: str, area_id: str, device_id: str, capability_type: str) -> str:
    return f"{DOMATOR_BASE}/{home_id}/{area_id}/{device_id}/{capability_type}/set"


# ── cover (blinds) extra topics ───────────────────────────────────────────────

def cover_position_topic(home_id: str, area_id: str, device_id: str) -> str:
    return f"{DOMATOR_BASE}/{home_id}/{area_id}/{device_id}/cover/position"


def cover_set_position_topic(home_id: str, area_id: str, device_id: str) -> str:
    return f"{DOMATOR_BASE}/{home_id}/{area_id}/{device_id}/cover/position/set"


# ── climate (heating) extra topics ───────────────────────────────────────────

def climate_mode_state_topic(home_id: str, area_id: str, device_id: str) -> str:
    return f"{DOMATOR_BASE}/{home_id}/{area_id}/{device_id}/climate/mode/state"


def climate_mode_command_topic(home_id: str, area_id: str, device_id: str) -> str:
    return f"{DOMATOR_BASE}/{home_id}/{area_id}/{device_id}/climate/mode/set"


def climate_target_temp_state_topic(home_id: str, area_id: str, device_id: str) -> str:
    return f"{DOMATOR_BASE}/{home_id}/{area_id}/{device_id}/climate/target/state"


def climate_target_temp_command_topic(home_id: str, area_id: str, device_id: str) -> str:
    return f"{DOMATOR_BASE}/{home_id}/{area_id}/{device_id}/climate/target/set"


def climate_current_temp_topic(home_id: str, area_id: str, device_id: str) -> str:
    return f"{DOMATOR_BASE}/{home_id}/{area_id}/{device_id}/climate/current/state"
