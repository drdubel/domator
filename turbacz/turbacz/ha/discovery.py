"""Home Assistant MQTT Discovery payload builders.

Payloads use HA's abbreviated keys (``cmd_t``, ``stat_t``, ``uniq_id``, ...) to
keep the retained configs small. Every builder is a pure function of its
arguments so the payloads can be asserted on in tests without a broker.
"""

from turbacz.ha import topics as T

MANUFACTURER = "drdubel"


# -- device blocks ------------------------------------------------------------


def section_device(section_id: int, section_name: str) -> dict:
    """Lights and covers group by section: one HA device per room."""
    return {
        "ids": [f"domator_section_{section_id}"],
        "name": section_name,
        "mf": MANUFACTURER,
        "mdl": "Domator Section",
        "sa": section_name,
    }


def switch_device(switch_id: int, switch_name: str) -> dict:
    """Buttons belong to a physical wall panel, not a room."""
    return {
        "ids": [f"domator_switch_{switch_id}"],
        "name": switch_name,
        "mf": MANUFACTURER,
        "mdl": "Domator Switch",
    }


def heating_device() -> dict:
    return {
        "ids": ["domator_heating"],
        "name": "Heating",
        "mf": MANUFACTURER,
        "mdl": "Domator Heating",
    }


# -- availability -------------------------------------------------------------


def availability(base: str, relay_id: int | None = None) -> dict:
    """Bridge availability, optionally ANDed with a relay board's own liveness."""
    topics = [{"t": T.bridge_status_topic(base)}]
    if relay_id is not None:
        topics.append({"t": T.relay_availability_topic(base, relay_id)})

    block: dict = {
        "avty": topics,
        "pl_avail": "online",
        "pl_not_avail": "offline",
    }
    if len(topics) > 1:
        block["avty_mode"] = "all"
    return block


# -- entities -----------------------------------------------------------------


def build_light(base: str, relay_id: int, output_id: str, name: str, device: dict) -> dict:
    key = T.output_key(relay_id, output_id)
    return {
        "name": name,
        "uniq_id": T.unique_id(T.LIGHT, key),
        "stat_t": T.state_topic(base, T.LIGHT, key),
        "cmd_t": T.command_topic(base, T.LIGHT, key),
        "pl_on": "ON",
        "pl_off": "OFF",
        **availability(base, relay_id),
        "dev": device,
    }


def build_cover(base: str, relay_id: int, power_id: str, name: str, device: dict) -> dict:
    """Blinds have no position feedback, so this is a state-only cover.

    No ``pos_t``/``set_pos_t``: the hardware cannot report where the blind is,
    and advertising a position HA cannot trust is worse than admitting there
    is none.
    """
    key = T.output_key(relay_id, power_id)
    return {
        "name": name,
        "uniq_id": T.unique_id(T.COVER, key),
        "stat_t": T.state_topic(base, T.COVER, key),
        "cmd_t": T.command_topic(base, T.COVER, key),
        "pl_open": "OPEN",
        "pl_cls": "CLOSE",
        "pl_stop": "STOP",
        "stat_open": "open",
        "stat_closed": "closed",
        "stat_opening": "opening",
        "stat_closing": "closing",
        "stat_stopped": "stopped",
        "dev_cla": "blind",
        **availability(base, relay_id),
        "dev": device,
    }


def build_water_heater(base: str) -> dict:
    """The mixing valve.

    ``min_temp`` must be set explicitly: HA's water_heater default is 43.3 degC,
    above a typical domestic mixing setpoint. ``mode_cmd_t`` is deliberately
    omitted -- the firmware has no mode concept, so HA must not offer one.
    """
    return {
        "name": "Water",
        "uniq_id": T.unique_id(T.WATER_HEATER, T.HEATING_KEY),
        "curr_temp_t": T.heating_current_topic(base),
        "temp_stat_t": T.heating_target_state_topic(base),
        "temp_cmd_t": T.heating_target_command_topic(base),
        "mode_stat_t": T.heating_mode_state_topic(base),
        "modes": ["performance"],
        "min_temp": 20,
        "max_temp": 60,
        "temp_unit": "C",
        "precision": 0.5,
        **availability(base),
        "dev": heating_device(),
    }


def build_sensor(
    base: str,
    key: str,
    name: str,
    *,
    unit: str | None = None,
    device_class: str | None = None,
    diagnostic: bool = False,
) -> dict:
    payload: dict = {
        "name": name,
        "uniq_id": T.unique_id(T.SENSOR, key),
        "stat_t": T.state_topic(base, T.SENSOR, key),
        "stat_cla": "measurement",
        **availability(base),
        "dev": heating_device(),
    }
    if unit:
        payload["unit_of_meas"] = unit
    if device_class:
        payload["dev_cla"] = device_class
    if diagnostic:
        payload["ent_cat"] = "diagnostic"
    return payload


def build_event(
    base: str, switch_id: int, button_id: str, name: str, event_types: list[str], device: dict
) -> dict:
    key = T.button_key(switch_id, button_id)
    return {
        "name": name,
        "uniq_id": T.unique_id(T.EVENT, key),
        "stat_t": T.state_topic(base, T.EVENT, key),
        "evt_typ": event_types,
        "dev_cla": "button",
        **availability(base),
        "dev": device,
    }
