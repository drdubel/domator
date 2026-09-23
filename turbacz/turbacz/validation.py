"""Command validation before database writes or MQTT topic construction."""

import math
import re


def integer(value, low=0, high=2**63 - 1):
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise ValueError("Expected an integer")  # noqa: TRY004
    if isinstance(value, str) and not re.fullmatch(r"[0-9]{1,19}", value):
        raise ValueError("Invalid integer")
    result = int(value)
    if isinstance(value, str) and str(result) != value:
        raise ValueError("Non-canonical integer")
    if not low <= result <= high:
        raise ValueError("Integer out of range")
    return result


def device_id(value):
    return integer(value, 1)


def output_id(value):
    if not isinstance(value, str) or not re.fullmatch(r"[a-p]", value):
        raise ValueError("Invalid output ID")
    return value


def button_id(value):
    # Mesh routing supports 24 logical buttons, including extended presses.
    if not isinstance(value, str) or not re.fullmatch(r"[a-x]", value):
        raise ValueError("Invalid button ID")
    return value


def name(value):
    if (
        not isinstance(value, str)
        or not value.strip()
        or len(value) > 64
        or any(ord(c) < 32 for c in value)
    ):
        raise ValueError("Invalid name")
    return value.strip()


def relay_output(cm, relay, output):
    relay, output = device_id(relay), output_id(output)
    if output not in cm.get_outputs().get(relay, {}):
        raise ValueError("Unknown relay output")
    return relay, output


def validate_command(path, cmd, cm):
    """Validate the complete message before the handler makes any changes.

    Keep the existing command format used by the browser, mobile app and mesh.
    Physical heating safety limits belong to the separate hardware review.
    """
    outputs = None

    def check_output(relay, output):
        nonlocal outputs
        relay, output = device_id(relay), output_id(output)
        if outputs is None:
            outputs = cm.get_outputs()
        if output not in outputs.get(relay, {}):
            raise ValueError("Unknown relay output")
        return relay, output

    if path.startswith("/heating/"):
        if not isinstance(cmd, str) or not re.fullmatch(
            r"[pidtI]-?(?:[0-9]{1,12})(?:\.[0-9]{1,12})?", cmd
        ):
            raise ValueError("Invalid heating command")
        if not math.isfinite(float(cmd[1:])):
            raise ValueError("Non-finite heating value")
        return
    if not isinstance(cmd, dict):
        raise ValueError("Expected an object")  # noqa: TRY004
    kind = cmd.get("type")
    if path.startswith("/blinds/"):
        if kind == "relay_blind_control":
            relay, power = check_output(cmd.get("relay_id"), cmd.get("power_id"))
            _, direction = check_output(relay, cmd.get("direction_id"))
            if [power, direction] not in [
                list(pair) for pair in cm.get_blind_pairs().get(relay, [])
            ]:
                raise ValueError("Unknown blind pair")
            if cmd.get("action") not in {"up", "down", "stop"}:
                raise ValueError("Invalid blind action")
        elif not isinstance(cmd.get("blind"), str) or not re.fullmatch(
            r"r[1-8]", cmd["blind"]
        ):
            raise ValueError("Invalid blind ID")
        else:
            integer(cmd.get("position"), 0, 999)
        return
    if path.startswith("/lights/") and kind in {
        "add_section",
        "change_section",
        "change_positions",
        "layout_update",
    }:
        if kind == "add_section":
            name(cmd.get("name"))
            return
        if kind == "change_section" or (
            kind == "layout_update" and cmd.get("section") is not None
        ):
            check_output(cmd.get("relay_id"), cmd.get("output_id"))
            if integer(cmd.get("section"), 1) not in cm.get_sections():
                raise ValueError("Unknown section")
        positions = cmd.get("positions", [])
        if not isinstance(positions, list):
            raise ValueError("Invalid positions")
        for position in positions:
            if not isinstance(position, dict):
                raise ValueError("Invalid position")  # noqa: TRY004
            check_output(position.get("relay_id"), position.get("output_id"))
            integer(position.get("output_idx"), 0, 4096)
        return
    if path.startswith("/rcm/"):
        if kind in {
            "update",
            "update_root",
            "update_all_relays",
            "update_all_switches",
            "get_states",
        }:
            return
        if kind == "update_device":
            if cmd.get("device_type") not in {"relay", "switch"}:
                raise ValueError("Invalid device type")
            identifier = device_id(cmd.get("device_id"))
            devices = (
                cm.get_relays() if cmd["device_type"] == "relay" else cm.get_switches()
            )
            if identifier not in devices:
                raise ValueError("Unknown device")
            return
        if kind == "auto_off_update":
            check_output(cmd.get("relay_id"), cmd.get("output_id"))
            integer(cmd.get("auto_off_seconds"), 0, 86400)
            return
        if kind == "button_types":
            data = cmd.get("data")
            if not isinstance(data, dict):
                raise ValueError("Invalid button types")
            known = cm.get_all_buttons()
            for switch, buttons in data.items():
                switch = device_id(switch)
                if not isinstance(buttons, dict):
                    raise ValueError("Invalid buttons")  # noqa: TRY004
                for button, state in buttons.items():
                    if button_id(button) not in known.get(switch, {}):
                        raise ValueError("Unknown button")
                    integer(state, 0, 1)
            return
    if kind is not None:
        raise ValueError("Unknown command")
    check_output(cmd.get("relay_id"), cmd.get("output_id"))
    integer(cmd.get("state"), 0, 1)
