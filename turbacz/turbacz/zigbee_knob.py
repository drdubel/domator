"""Stable RCM button IDs for the Tuya knob, in event and command modes."""

from turbacz.zigbee_devices import is_zigbee_id

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
        switch: value for switch, value in data.items() if not is_zigbee_id(int(switch))
    }
