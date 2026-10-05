"""Current configuration is authoritative, so role changes revoke old tokens."""

from turbacz.settings import config


def user_roles(subject):
    return config.roles.get(subject, {"viewer"})


def permitted(user, capability):
    if not user or user.get("sub") not in config.authorized:
        return False
    roles = user_roles(user["sub"])
    return (
        "admin" in roles
        or capability in roles
        or (capability == "viewer" and bool(roles))
    )


def command_capability(path, command):
    if isinstance(command, dict):
        kind = command.get("type")
        if kind in {"ping", "get_states"}:
            return "viewer"
        if kind in {
            "update_root",
            "update_all_relays",
            "update_all_switches",
            "update_device",
        }:
            return "ota"
        if kind in {
            "add_section",
            "change_section",
            "change_positions",
            "layout_update",
            "gateway_mode",
            "auto_off_update",
            "update",
            "button_types",
            "zigbee_knob_command",
        }:
            return "admin"
    if path.startswith(("/heating/", "/blinds/", "/lights/", "/rcm/")):
        return "operator"
    return "admin"
