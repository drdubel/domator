"""Topic and unique-ID construction for the Home Assistant MQTT bridge.

Every name here is keyed only on *immutable* identity -- ``(relay_id,
output_id)``, ``switch_id`` -- and never on the section an entity currently
belongs to. Moving a light to another room therefore rewrites its discovery
payload but leaves its state topic, and so its Home Assistant history, alone.

This module is deliberately free of config and I/O imports so it stays
trivially unit-testable.
"""

LIGHT = "light"
COVER = "cover"
WATER_HEATER = "water_heater"
SENSOR = "sensor"
EVENT = "event"

HEATING_KEY = "heating"


# -- keys ---------------------------------------------------------------------


def output_key(relay_id: int, output_id: str) -> str:
    """Identity of a single relay output, used for lights and covers."""
    return f"{relay_id}_{output_id}"


def button_key(switch_id: int, button_id: str) -> str:
    return f"{switch_id}_{button_id}"


def unique_id(component: str, key: str) -> str:
    """Stable ``unique_id``; also used as the discovery object id."""
    return f"domator_{component}_{key}"


# -- discovery ----------------------------------------------------------------


def discovery_topic(prefix: str, component: str, key: str) -> str:
    return f"{prefix}/{component}/{unique_id(component, key)}/config"


# -- availability -------------------------------------------------------------


def bridge_status_topic(base: str) -> str:
    return f"{base}/status"


def relay_availability_topic(base: str, relay_id: int) -> str:
    return f"{base}/availability/relay_{relay_id}"


# -- state / command ----------------------------------------------------------


def state_topic(base: str, component: str, key: str) -> str:
    return f"{base}/{component}/{key}/state"


def command_topic(base: str, component: str, key: str) -> str:
    return f"{base}/{component}/{key}/set"


# -- heating ------------------------------------------------------------------


def heating_current_topic(base: str) -> str:
    return f"{base}/{WATER_HEATER}/{HEATING_KEY}/current"


def heating_target_state_topic(base: str) -> str:
    return f"{base}/{WATER_HEATER}/{HEATING_KEY}/target/state"


def heating_target_command_topic(base: str) -> str:
    return f"{base}/{WATER_HEATER}/{HEATING_KEY}/target/set"


def heating_mode_state_topic(base: str) -> str:
    return f"{base}/{WATER_HEATER}/{HEATING_KEY}/mode/state"


def heating_sensor_key(probe: str) -> str:
    return f"{HEATING_KEY}_{probe}"
