"""Validate complete telemetry messages before any state changes."""

import json
import math
import re

from turbacz.settings import config


def number(value, low, high):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TypeError("Expected a number")
    if not math.isfinite(value) or not low <= value <= high:
        raise ValueError("Number outside range")


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError("Expected a bounded integer")


def validate_payload(topic, payload):
    if topic.startswith(f"{config.zigbee.base_topic}/"):
        from turbacz.zigbee_devices import validate_zigbee_payload

        validate_zigbee_payload(topic, payload)
    elif topic == "/heating/metrics":
        data = json.loads(payload)
        if not isinstance(data, dict):
            raise ValueError("Expected an object")
        for field in (
            "cold",
            "mixed",
            "hot",
            "integral",
            "pid_output",
            "target",
            "kp",
            "ki",
            "kd",
        ):
            number(data[field], -1e9, 1e9)
        if set(data) - {
            "cold",
            "mixed",
            "hot",
            "integral",
            "pid_output",
            "target",
            "kp",
            "ki",
            "kd",
        }:
            raise ValueError("Unknown heating metric")
    elif topic == "/blind/pos":
        if not re.fullmatch(r"r[1-8] (?:[0-9]|[1-9][0-9]|100)", payload):
            raise ValueError("Invalid blind position")
    elif topic == "/switch/state/root":
        data = json.loads(payload)
        if not isinstance(data, dict):
            raise ValueError("Expected an object")
        if data.get("status") in {"connected", "disconnected"}:
            return
        if data["type"] not in {"switch", "remote", "relay8", "relay16"}:
            raise ValueError("Unknown device type")
        integer(data["deviceId"], 1, 2**63 - 1)
        integer(data["parentId"], 0, 2**63 - 1)
        for key in ("uptime", "clicks", "freeHeap"):
            integer(data[key], 0, 2**53 - 1)
        integer(data["rssi"], -128, 0)
        integer(data["firmware"], 0, 2**53 - 1)
        if "isRoot" in data and data["isRoot"] not in (0, 1):
            raise ValueError("Invalid root flag")
        if "gateway" in data and type(data["gateway"]) is not bool:
            raise ValueError("Invalid gateway flag")
        if "gatewayMac" in data and not re.fullmatch(
            r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", data["gatewayMac"]
        ):
            raise ValueError("Invalid gateway address")
        if "radioChannel" in data:
            integer(data["radioChannel"], 0, 14)
    elif topic.startswith(("/relay/state/", "/switch/state/")):
        suffix = topic.rsplit("/", 1)[-1]
        if not re.fullmatch(r"[0-9]{1,19}", suffix):
            raise ValueError("Invalid device topic")
        integer(int(suffix), 1, 2**63 - 1)
        if topic.startswith("/relay/state/"):
            if not re.fullmatch(r"[A-P][01]", payload):
                raise ValueError("Invalid relay state")
        elif not re.fullmatch(r"[a-x](?:[01])?|[0-9]{1,6}", payload):
            raise ValueError("Invalid switch state")
