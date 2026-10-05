"""Execute the knob's RCM connections in the backend, outside the mesh."""

from turbacz.connection_manager import connection_manager
from turbacz.database import db_call
from turbacz.ha.bridge import ha_bridge
from turbacz.mqtt_client import mqtt, publish_blind_action
from turbacz.websocket import ws_manager
from turbacz.zigbee_knob import (
    KNOB_ACTION_BUTTONS,
    KNOB_SWITCH_ID,
    validate_knob_payload,
)


async def handle_knob_message(payload: str) -> None:
    data = validate_knob_payload(payload)
    button_id = KNOB_ACTION_BUTTONS.get(data.get("action"))
    if button_id is None:
        return

    await ws_manager.broadcast(
        {"type": "switch_state", "switch_id": KNOB_SWITCH_ID, "button_id": button_id},
        "/rcm/ws/",
    )
    await ha_bridge.on_button_event(KNOB_SWITCH_ID, button_id, "press")

    connections = await db_call(connection_manager.get_all_connections)
    targets = connections.get(KNOB_SWITCH_ID, {}).get(button_id, [])
    if not targets:
        return
    knob = await db_call(connection_manager.get_zigbee_knob_config)
    command = knob["buttons"][button_id]["command"]
    outputs = await db_call(connection_manager.get_outputs)
    pairs = await db_call(connection_manager.get_blind_pairs)
    planned = []
    # Validate the entire routing plan before changing any output.
    for relay_id, output_id in targets:
        if output_id not in outputs.get(relay_id, {}):
            raise ValueError("Unknown knob output")
        pair = next(
            (pair for pair in pairs.get(relay_id, []) if output_id in pair), None
        )
        if command in {"up", "down", "stop"}:
            if pair is None or any(oid not in outputs[relay_id] for oid in pair):
                raise ValueError("Knob blind command requires a registered blind pair")
            entry = (relay_id, tuple(pair))
        else:
            if pair is not None:
                raise ValueError("Use up, down or stop for knob blind connections")
            entry = (relay_id, output_id)
        if entry not in planned:
            planned.append(entry)

    for relay_id, target in planned:
        if command in {"up", "down", "stop"}:
            publish_blind_action(mqtt.client, relay_id, target[0], target[1], command)
        else:
            # Relay firmware handles toggle atomically, without stale state.
            suffix = {"on": "1", "off": "0", "toggle": ""}[command]
            mqtt.client.publish(f"/relay/cmd/{relay_id}", f"{target}{suffix}")
