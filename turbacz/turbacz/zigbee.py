"""Discover Zigbee2MQTT devices and execute their RCM action connections."""

from turbacz.connection_manager import connection_manager
from turbacz.database import db_call
from turbacz.ha.bridge import ha_bridge
from turbacz.mqtt_client import mqtt, publish_blind_action
from turbacz.settings import config
from turbacz.websocket import ws_manager
from turbacz.zigbee_devices import action_values, validate_zigbee_payload, zigbee_topic


async def handle_zigbee_message(
    topic: str, payload: str, retained: bool = False
) -> None:
    data = validate_zigbee_payload(topic, payload)
    if data is None:
        return
    if topic == config.zigbee.base_topic + "/bridge/devices":
        changed = False
        for entry in data:
            if entry.get("type") == "Coordinator":
                continue
            device_topic = config.zigbee.base_topic + "/" + entry["friendly_name"]
            if zigbee_topic(device_topic) != (device_topic, "state"):
                continue
            _, updated = await db_call(
                connection_manager.register_zigbee_device,
                device_topic,
                action_values((entry.get("definition") or {}).get("exposes", [])),
            )
            changed |= updated
        if changed:
            ha_bridge.schedule_resync()
            await ws_manager.broadcast({"type": "update"}, "/rcm/ws/")
        return

    device_topic, kind = zigbee_topic(topic)
    action = data.get("action") if kind == "state" else None
    device, changed = await db_call(
        connection_manager.register_zigbee_device,
        device_topic,
        [action] if action else [],
    )
    if changed:
        ha_bridge.schedule_resync()
        await ws_manager.broadcast({"type": "update"}, "/rcm/ws/")
    await ws_manager.broadcast(
        {"type": "zigbee_state", "switch_id": device["switch_id"], "state": data},
        "/rcm/ws/",
    )
    # Retained messages discover devices and capabilities but cannot replay
    # a physical action. Metadata and availability never execute connections.
    if retained or not action or action not in device["actions"]:
        return
    await handle_device_action(device, action)


async def handle_device_action(device: dict, action: str) -> None:
    switch_id = device["switch_id"]
    button_id = device["actions"][action]
    await ws_manager.broadcast(
        {"type": "switch_state", "switch_id": switch_id, "button_id": button_id},
        "/rcm/ws/",
    )
    await ha_bridge.on_button_event(switch_id, button_id, "press")
    connections = await db_call(connection_manager.get_all_connections)
    targets = connections.get(switch_id, {}).get(button_id, [])
    if not targets:
        return
    command = device["buttons"][button_id]["command"]
    outputs = await db_call(connection_manager.get_outputs)
    pairs = await db_call(connection_manager.get_blind_pairs)
    planned = []
    # Validate the entire routing plan before changing any output.
    for relay_id, output_id in targets:
        if output_id not in outputs.get(relay_id, {}):
            raise ValueError("Unknown Zigbee action output")
        pair = next(
            (pair for pair in pairs.get(relay_id, []) if output_id in pair), None
        )
        if command in {"up", "down", "stop"}:
            if pair is None or any(oid not in outputs[relay_id] for oid in pair):
                raise ValueError("Zigbee blind command requires a registered blind pair")
            entry = (relay_id, tuple(pair))
        else:
            if pair is not None:
                raise ValueError("Use up, down or stop for Zigbee blind connections")
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
