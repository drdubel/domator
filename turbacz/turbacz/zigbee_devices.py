"""Zigbee2MQTT topic classification and stable virtual device identities."""

import hashlib
import json

from turbacz.settings import config

ZIGBEE_ID_START = 1 << 48
ZIGBEE_ID_END = (1 << 49) - 1


def zigbee_device_id(topic: str) -> int:
    return ZIGBEE_ID_START + int.from_bytes(
        hashlib.blake2b(topic.encode(), digest_size=6).digest(), "big"
    )


def is_zigbee_id(device_id: int) -> bool:
    return ZIGBEE_ID_START <= device_id <= ZIGBEE_ID_END


def zigbee_topic(topic: str) -> tuple[str, str] | None:
    prefix = config.zigbee.base_topic + "/"
    if not topic.startswith(prefix):
        return None
    name = topic[len(prefix) :]
    if not name or name == "bridge" or name.startswith("bridge/"):
        return None
    parts = name.split("/")
    # Friendly names may contain folders, but these suffixes are Z2M protocol
    # endpoints, not additional devices. Ignore /set/state and /get/... too.
    if any(part in {"set", "get"} for part in parts):
        return None
    if parts[-1] == "action":
        return None
    if parts[-1] == "availability":
        kind = parts.pop()
        name = "/".join(parts)
        return (prefix + name, kind) if name else None
    return topic, "state"


def action_values(exposes) -> list[str]:
    actions = []
    if isinstance(exposes, list):
        for expose in exposes:
            actions.extend(action_values(expose))
    elif isinstance(exposes, dict):
        if exposes.get("property") == "action":
            values = exposes.get("values", [])
            if isinstance(values, list):
                actions.extend(
                    value for value in values if isinstance(value, str) and value
                )
        actions.extend(action_values(exposes.get("features", [])))
    return list(dict.fromkeys(actions))


def validate_zigbee_payload(topic: str, payload: str):
    if topic == config.zigbee.base_topic + "/bridge/devices":
        data = json.loads(payload)
        if not isinstance(data, list):
            raise TypeError("Expected a Zigbee2MQTT device list")
        for device in data:
            if (
                not isinstance(device, dict)
                or not isinstance(device.get("friendly_name"), str)
                or not device["friendly_name"]
            ):
                raise ValueError("Invalid Zigbee2MQTT device")
            if device.get("definition") is not None and not isinstance(
                device["definition"], dict
            ):
                raise ValueError("Invalid Zigbee2MQTT device definition")
            for action in action_values(
                (device.get("definition") or {}).get("exposes", [])
            ):
                validate_action(action)
        return data
    endpoint = zigbee_topic(topic)
    if endpoint is None:
        return None
    _, kind = endpoint
    if kind == "availability" and payload in ("online", "offline"):
        return {"state": payload}
    data = json.loads(payload)
    if kind == "availability" and data in ("online", "offline"):
        return {"state": data}
    if not isinstance(data, dict):
        raise TypeError("Expected a Zigbee2MQTT object")
    if kind == "availability":
        if data.get("state") not in ("online", "offline"):
            raise ValueError("Invalid Zigbee2MQTT availability")
    elif data.get("action") is not None:
        validate_action(data["action"])
    return data


def validate_action(action):
    if not isinstance(action, str):
        raise TypeError("Expected a string action")
    if len(action) > 128 or any(ord(c) < 32 for c in action):
        raise ValueError("Invalid Zigbee action")
