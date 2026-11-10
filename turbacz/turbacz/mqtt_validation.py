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


def gates(values, high):
    if not isinstance(values, list) or len(values) != 16:
        raise ValueError("Expected one value per range gate")
    for value in values:
        integer(value, 0, high)


def validate_presence_report(data):
    """Radar configuration or live data from an mmWave presence sensor."""
    if data.get("type") != "presence" or data.get("report") not in {"config", "live"}:
        raise ValueError("Unknown device report")
    integer(data["deviceId"], 1, 2**63 - 1)
    if data["report"] == "live":
        integer(data["presence"], 0, 1)
        integer(data["distance"], 0, 65535)
        gates(data["energy"], 65535)
        if "calibrating" in data:
            integer(data["calibrating"], 0, 120)
        return
    integer(data["min"], 0, 15)
    integer(data["max"], 0, 15)
    integer(data["delay"], 0, 65535)
    integer(data["led"], 0, 1)
    gates(data["trigger"], 2**32 - 1)
    gates(data["hold"], 2**32 - 1)
    if not isinstance(data.get("fw", ""), str) or not re.fullmatch(r"[ -~]{0,15}", data.get("fw", "")):
        raise ValueError("Invalid radar firmware version")


def normalize_payload(topic, payload):
    """Accept the single C-string terminator emitted by older mesh roots.

    Limit compatibility to button frames; embedded/repeated NUL bytes and
    other telemetry must still fail their normal validation.
    """
    if (
        topic.startswith("/switch/state/")
        and topic != "/switch/state/root"
        and re.fullmatch(r"[a-x](?:[01])?\x00", payload)
    ):
        return payload[:-1]
    return payload


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
        if "report" in data:
            validate_presence_report(data)
            return
        if data["type"] not in {"switch", "remote", "relay8", "relay16", "presence"}:
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
        for key in ("presence", "radar"):
            if key in data:
                integer(data[key], 0, 1)
        if "distance" in data:
            integer(data["distance"], 0, 65535)
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
