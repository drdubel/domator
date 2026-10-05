"""Stable RCM button IDs for the Tuya knob, in event and command modes."""

import json

# Mesh device IDs are 48-bit MAC addresses. Reserve the first ID above that
# range for this virtual switch; it is still exactly representable in JS.
KNOB_SWITCH_ID = 1 << 48
KNOB_BUTTONS = {
    "a": ("Click", "toggle"),
    "b": ("Double-click", "toggle"),
    "c": ("Hold", "stop"),
    "d": ("Rotate left", "off"),
    "e": ("Rotate right", "on"),
    "f": ("Hold + rotate left", "up"),
    "g": ("Hold + rotate right", "down"),
    "h": ("Release hold", "stop"),
    "i": ("Saturation move", "toggle"),
}
KNOB_ACTION_BUTTONS = {
    "single": "a",
    "toggle": "a",
    "double": "b",
    "hold": "c",
    "hue_move": "c",
    "rotate_left": "d",
    "brightness_step_down": "d",
    "rotate_right": "e",
    "brightness_step_up": "e",
    "color_temperature_step_down": "f",
    "color_temperature_step_up": "g",
    "hue_stop": "h",
    "saturation_move": "i",
}
KNOB_COMMANDS = {"toggle", "on", "off", "up", "down", "stop"}


def mesh_switch_config(data: dict) -> dict:
    """Virtual knob routing belongs to Turbacz, not the ESP32 mesh root."""
    return {
        switch: value
        for switch, value in data.items()
        if str(switch) != str(KNOB_SWITCH_ID)
    }


def validate_knob_payload(payload: str) -> dict:
    data = json.loads(payload)
    if not isinstance(data, dict):
        raise TypeError("Expected a Zigbee2MQTT object")
    # Battery/linkquality reports and action resets carry no usable action.
    if (
        "action" in data
        and data["action"] is not None
        and not isinstance(data["action"], str)
    ):
        raise TypeError("Expected a string action")
    return data
