# Phase 4 - Root Routing Logic Testing Guide

## Overview
This document describes how to test the Phase 4 routing logic implementation for the buttonsMeshIDF project.

## Features Implemented

### 1. Connection Map Parsing (#13)
**What it does:** Parses JSON configuration that maps button presses to relay commands.

**MQTT Config Format:**
```json
{
  "type": "connections",
  "data": {
    "12345678": {
      "a": [
        ["87654321", "a"],
        ["11223344", "b"]
      ],
      "b": [
        ["87654321", "c1"]
      ]
    }
  }
}
```

**How to test:**
1. Set up root node with MQTT broker
2. Publish to `/switch/cmd/root` with the above JSON
3. Check logs for "Parsing connections configuration" and "Connections parsed successfully"
4. Verify device/button mappings are logged

### 2. Button Type Configuration (#14)
**What it does:** Configures button behavior (0=toggle, 1=stateful).

**MQTT Config Format:**
```json
{
  "type": "button_types",
  "data": {
    "12345678": {
      "a": 0,
      "b": 1,
      "c": 0
    }
  }
}
```

**How to test:**
1. Publish to `/switch/cmd/root` with the above JSON
2. Check logs for "Parsing button types configuration" and "Button types parsed successfully"
3. Verify button type assignments are logged

### 3. Button → Relay Routing (#15)
**What it does:** Routes button presses from switch nodes to relay nodes based on connection map.

**How to test:**
1. Configure connection map (see #13)
2. Configure button types (see #14)
3. Press button 'a' on switch node (device 12345678)
4. Check root logs for:
   - "Button 'a' pressed on device 12345678"
   - "Routing button 'a' from device 12345678"
   - "Found N routing targets"
   - "Routing to node 87654321: a"
5. Verify relay nodes receive and execute commands

**Toggle vs Stateful:**
- Toggle (type=0): Command sent as-is (e.g., "a" → "a")
- Stateful (type=1): State appended (e.g., "a" + state → "a0" or "a1")

### 4. MQTT → Node Forwarding (#16)
**What it does:** Forwards MQTT commands to mesh nodes.

**Already implemented in Phase 3**, now enhanced with config support.

**How to test:**
1. Publish "a" to `/relay/cmd/87654321`
2. Check root logs for command processing
3. Verify relay node receives command
4. Check relay toggles and publishes state confirmation

### 5. Relay State → MQTT (#17)
**What it does:** Publishes relay state changes to MQTT.

**Already implemented in Phase 3**.

**How to test:**
1. Toggle relay on relay node
2. Check MQTT topic `/relay/state/{deviceId}/a` for state ("0" or "1")

### 6. Node Status → MQTT (#18)
**What it does:** Forwards node status reports from mesh to MQTT.

**Already implemented in Phase 1**.

**How to test:**
1. Wait for status reports (every 15 seconds)
2. Check MQTT topic `/switch/state/root` for JSON status
3. Verify leaf node status includes `parentId` field

### 7. MQTT Config Receive (#19)
**What it does:** Receives and processes configuration via MQTT.

**How to test:**
1. Subscribe root to `/switch/cmd/root`
2. Send connection map config (see #13)
3. Send button types config (see #14)
4. Verify configs are parsed and applied
5. Test button press routing to confirm config is active

## Integration Test Scenario

### Setup:
- 1 Root node (device ID: 10000001)
- 1 Switch node (device ID: 20000002) with buttons a-g
- 2 Relay nodes:
  - Relay 1 (device ID: 30000003) with 8 relays
  - Relay 2 (device ID: 30000004) with 8 relays

### Configuration:
```json
// Connection map
{
  "type": "connections",
  "data": {
    "20000002": {
      "a": [["30000003", "a"]],
      "b": [["30000003", "b"], ["30000004", "a"]],
      "c": [["30000004", "c1"]]
    }
  }
}

// Button types
{
  "type": "button_types",
  "data": {
    "20000002": {
      "a": 0,
      "b": 0,
      "c": 1
    }
  }
}
```

### Test Steps:
1. Send both configs to `/switch/cmd/root`
2. Press button 'a' on switch → Relay 1 relay 'a' should toggle
3. Press button 'b' on switch → Relay 1 relay 'b' AND Relay 2 relay 'a' should toggle
4. Press button 'c' on switch → Relay 2 relay 'c' should turn ON (stateful)
5. Release button 'c' on switch → Relay 2 relay 'c' should turn OFF (stateful)
6. Verify all relay state changes are published to MQTT

### Expected MQTT Traffic:
- `/switch/state/20000002` - Button press events ('a', 'b', 'c')
- `/relay/state/30000003/a` - Relay 1 state changes ('0' or '1')
- `/relay/state/30000003/b` - Relay 1 state changes ('0' or '1')
- `/relay/state/30000004/a` - Relay 2 state changes ('0' or '1')
- `/relay/state/30000004/c` - Relay 2 state changes ('0' or '1')

## Known Limitations

1. **Device Addressing:** Commands are broadcast to all nodes. Nodes filter based on their device ID. Direct MAC-based addressing not implemented.

2. **Memory Management:** Dynamic allocation used for routing targets. Max 50 devices and 10 routes per button.

3. **Configuration Persistence:** Config stored in RAM only. Lost on reboot. Consider adding NVS storage in future.

4. **Concurrency:** Mutexes protect routing tables, but commands may arrive out of order under high load.

## Debugging Tips

1. **Enable verbose logging:** Set log level to DEBUG in menuconfig
2. **Monitor heap usage:** Check logs for "low heap" warnings
3. **Check mutex timeouts:** Look for "Failed to acquire mutex" errors
4. **Verify JSON parsing:** If config not working, check JSON syntax
5. **Device ID mismatch:** Ensure device IDs in config match actual node IDs

## Performance Notes

- Button press routing: ~10ms overhead
- Config parsing: ~100-500ms depending on size
- Memory usage: ~200 bytes per routing target
- Max config size: Limited by MQTT message size (~200 bytes per message)

## Future Enhancements

1. Add NVS storage for persistent config
2. Implement direct MAC-based addressing for better performance
3. Add config version tracking and synchronization
4. Support for more complex routing rules (conditions, groups, scenes)
5. Web UI for configuration management
