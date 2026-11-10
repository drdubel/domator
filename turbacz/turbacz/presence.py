"""Waveshare HMMD mmWave presence sensors (ESP32-C3 SuperMini, switch_role=presence).

A sensor reports presence on three inputs that RCM wires like switch buttons.
Each has a fixed routing type, understood by the mesh root:
  a: outputs follow presence (on while occupied, off when clear)
  b: outputs turn on when presence starts
  c: outputs turn off when the area becomes clear
Range gates, the absence delay and per-gate thresholds are stored in the
radar itself. Turbacz keeps the configuration the sensor last reported.
"""

import json

GATES = 16
PRESENCE_BUTTONS = {"a": 2, "b": 3, "c": 4}
CONFIG_KEYS = ("min", "max", "delay", "trigger", "hold", "led")


def report_config(data: dict) -> dict:
    """The cached form of a validated ``{"report": "config"}`` status."""
    return {
        "min": data["min"],
        "max": data["max"],
        "delay": data["delay"],
        "trigger": list(data["trigger"]),
        "hold": list(data["hold"]),
        "led": bool(data["led"]),
        "fw": data.get("fw", ""),
    }


def command_payload(cmd: dict) -> str:
    """Firmware command for a validated RCM ``presence_command``."""
    action = cmd["action"]
    payload = {"radar": action}
    if action == "apply":
        config = cmd["config"]
        payload.update(
            min=int(config["min"]),
            max=int(config["max"]),
            delay=int(config["delay"]),
            trigger=[int(value) for value in config["trigger"]],
            hold=[int(value) for value in config["hold"]],
            led=int(config["led"]),
        )
    elif action in {"calibrate", "live"}:
        payload["seconds"] = int(cmd["seconds"])
    return json.dumps(payload, separators=(",", ":"))
