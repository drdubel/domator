# Leaf Node Status Reporting Guide

## Overview

In ESP-WIFI-MESH, **only the root node** connects to MQTT. Leaf nodes (non-root nodes) send their status to the root through the mesh network, and the root forwards this information to MQTT.

## How It Works

### Root Node
- Gets IP address from WiFi router
- Connects to MQTT broker
- Publishes its own status to MQTT topics
- Receives status from leaf nodes via mesh
- Forwards leaf node status to MQTT

### Leaf Nodes (Switches, Relays)
- Connect only to mesh network (no direct WiFi/MQTT)
- Send status periodically to root via mesh
- Send button presses/relay changes to root via mesh
- Root handles all MQTT communication

## Status Report Flow

```
Leaf Node → Mesh Network → Root Node → MQTT Broker → Home Automation
```

### Message Flow Example

**Leaf Switch (Device 171447004):**
1. Connects to mesh parent
2. Every 20 seconds, creates status JSON:
   ```json
   {
     "deviceId": 171447004,
     "type": "switch",
     "freeHeap": 92000,
     "uptime": 300,
     "firmware": "43c3bea",
     "clicks": 5,
     "rssi": -45,
     "disconnects": 0,
     "lowHeap": 0
   }
   ```
3. Queues message to root via mesh
4. Root receives and publishes to MQTT topic: `/switch/state/<device_id>`

## Expected Log Patterns

### Leaf Node (Correct Behavior)

```
I (3110) NODE_SWITCH: Button task started with gesture support
I (3122) HEALTH_OTA: Root loss check task started
I (3127) HEALTH_OTA: Health monitor task started
I (3131) DOMATOR_MESH: Domator Mesh initialized
I (5177) MESH_INIT: find root:ESPM_3812DC, root_cap:2(max:300)
I (8500) MESH_INIT: ✓ Parent connected - Layer: 2, Mesh connected, status reports will be sent to root
I (8500) MESH_COMM: Status report task started
I (13500) MESH_COMM: Status report sent to root (count: 1): {"deviceId":171447004,...}
I (33500) MESH_COMM: Status report sent to root (count: 2): {"deviceId":171447004,...}
```

**Key indicators:**
- ✅ "Parent connected" with layer > 1 (not root)
- ✅ "Status report sent to root" messages
- ✅ NO "MQTT" initialization messages
- ✅ NO "Root got IP" messages

### Root Node (For Comparison)

```
I (7245) MESH_INIT: Root got IP: 192.168.1.45
I (7246) NODE_ROOT: ✓ Initializing MQTT client (ROOT node, device_id: 1074205304)
I (7251) NODE_ROOT: MQTT client started
I (8123) NODE_ROOT: MQTT connected
I (8500) MESH_COMM: Status report task started
```

**Key indicators:**
- ✅ "Root got IP" message
- ✅ "Initializing MQTT client (ROOT node)" message
- ✅ "MQTT connected" message
- ✅ Layer: 1

## Troubleshooting

### Issue: "Not connected to mesh, skipping status report"

**Symptoms:**
```
W (13500) MESH_COMM: Not connected to mesh, skipping status report (skipped: 1)
W (33500) MESH_COMM: Not connected to mesh, skipping status report (skipped: 10)
```

**Cause:** Node hasn't successfully connected to mesh parent

**Solutions:**
1. **Check WiFi Configuration**
   - Verify `WIFI_SSID` and `WIFI_PASSWORD` are correct
   - Ensure 2.4GHz WiFi is enabled on router
   - Check WiFi signal strength

2. **Check Mesh Configuration**
   - Verify `MESH_ID` matches across all devices
   - Check `MESH_MAX_LAYER` (default: 6)
   - Ensure at least one device can become root (good WiFi signal)

3. **Check for Mesh Issues**
   - Look for "Parent connected" in logs
   - Check if root node is operational
   - Verify no mesh-level errors

### Issue: Leaf Node Trying to Connect to MQTT

**Symptoms:**
```
W (xxxx) NODE_ROOT: ❌ MQTT init called on NON-ROOT node (device_id: 171447004, layer: 2) - skipping
W (xxxx) NODE_ROOT:    This is expected for leaf nodes. Only root connects to MQTT.
```

**This is NORMAL!** The log message indicates:
- The guard is working correctly
- The node correctly identified itself as non-root
- MQTT initialization was skipped (as intended)
- No actual MQTT connection attempt was made

**If you see these logs frequently:**
- IP_EVENT_STA_GOT_IP might be firing on leaf nodes (unusual)
- Check mesh configuration
- Ensure only one root in the network

### Issue: No Status Reports Visible

**Possible causes:**

1. **Mesh Not Connected**
   - Wait for "Parent connected" log
   - Check mesh is properly formed

2. **Logging Level Too High**
   - Status reports use INFO level
   - Check menuconfig → Component config → Log output → Default log verbosity

3. **Root Not Receiving**
   - Check root node MQTT connection
   - Verify root is publishing to MQTT
   - Check broker logs

4. **Queue Full**
   - Look for "TX queue full" warnings
   - Indicates network congestion

## Verification Steps

### 1. Verify Mesh Connection

**On leaf node:**
```bash
# Look for these logs
grep "Parent connected" monitor.log
grep "Mesh connected" monitor.log
```

Expected: Should see "Parent connected" with layer > 1

### 2. Verify Status Reports

**On leaf node:**
```bash
# Look for status reports
grep "Status report sent" monitor.log
```

Expected: Should see periodic messages every 20 seconds

### 3. Verify Root Receives

**On root node:**
```bash
# Look for MQTT publishes
grep "Published status" monitor.log
```

Expected: Should see publishes for leaf node device IDs

### 4. Verify MQTT Broker

```bash
# Subscribe to all status topics
mosquitto_sub -t "/switch/state/#" -t "/relay/state/#" -v
```

Expected: Should see status messages from all nodes

## Configuration

### Status Report Interval

Default: 20 seconds (20000 ms)

**To change:**
```c
// In domator_mesh.h
#define STATUS_REPORT_INTERVAL_MS  20000
```

### Mesh Configuration

**menuconfig:**
```
→ Domator Mesh Configuration
  → Mesh ID: "ESPM" (must match all devices)
  → Max Layer: 6 (default)
  → WiFi SSID: "YourNetwork"
  → WiFi Password: "YourPassword"
```

## Status Message Format

### Switch Node Status
```json
{
  "deviceId": 171447004,
  "type": "switch",
  "freeHeap": 92000,
  "uptime": 300,
  "firmware": "43c3bea",
  "clicks": 5,
  "rssi": -45,
  "disconnects": 0,
  "lowHeap": 0
}
```

### Relay Node Status
```json
{
  "deviceId": 1074207536,
  "type": "relay",
  "freeHeap": 85000,
  "uptime": 450,
  "firmware": "43c3bea",
  "outputs": 8,
  "rssi": -50,
  "disconnects": 0,
  "lowHeap": 0
}
```

## Architecture

```
┌─────────────────┐
│  Home Assistant │
│   MQTT Broker   │
└────────┬────────┘
         │ MQTT
         │
    ┌────▼────┐
    │  Root   │ Layer 1
    │ (WiFi)  │ - IP: 192.168.1.45
    │ Device  │ - MQTT connected
    └────┬────┘
         │ Mesh
    ┌────▼────┐
    │ Switch  │ Layer 2
    │ #171447 │ - No WiFi/IP
    └────┬────┘ - Sends to root
         │ Mesh
    ┌────▼────┐
    │ Switch  │ Layer 3
    │ #180234 │ - No WiFi/IP
    └─────────┘ - Sends to root
```

## Best Practices

1. **Only one root per mesh**
   - ESP-WIFI-MESH automatically elects root
   - Root has best WiFi signal
   - Other devices become leaf nodes

2. **Status reporting is automatic**
   - No manual configuration needed
   - Starts automatically after mesh connection
   - Continues every 20 seconds

3. **Monitor via MQTT**
   - Subscribe to `/switch/state/#` and `/relay/state/#`
   - Check for all expected device IDs
   - Monitor timestamps for health

4. **Leaf nodes DON'T need MQTT config**
   - MQTT broker URL only matters for root
   - Leaf nodes never connect directly to MQTT
   - Root handles all MQTT communication

## Summary

✅ **Leaf nodes DO:**
- Connect to mesh network
- Send status to root via mesh
- Send button/relay events to root
- Report health metrics to root

❌ **Leaf nodes DON'T:**
- Connect to WiFi router (mesh only)
- Get IP address
- Connect to MQTT broker
- Need MQTT configuration

🎯 **Root node does:**
- Connect to WiFi router
- Get IP via DHCP
- Connect to MQTT broker
- Publish all status to MQTT
- Forward leaf node messages to MQTT
