# ESP-WIFI-MESH Root Election Process

## Overview

The Domator mesh network uses ESP-WIFI-MESH, which automatically elects one device as the **root node**. The root node is special because:

- It connects directly to the WiFi router (not just the mesh)
- It gets an IP address via DHCP
- It handles MQTT communication with external services
- It forwards commands from MQTT to mesh nodes
- It publishes status from mesh nodes to MQTT

All other devices become **leaf nodes** that communicate through the mesh network.

## How Root Election Works

### Automatic Election Process

ESP-WIFI-MESH uses an automatic root election algorithm:

```
1. All devices start scanning for the mesh network
2. Devices exchange information about their connectivity
3. Root election considers:
   - Signal strength (RSSI) to WiFi router
   - Network connectivity quality
   - Vote percentage threshold (90% default)
4. Device with best router connection becomes root
5. Root obtains IP address from WiFi router via DHCP
6. Root initializes MQTT connection
7. Other devices connect as leaf nodes through mesh
```

### Election Criteria

The root node is elected based on:

1. **WiFi Router Connectivity**
   - Strongest RSSI to the WiFi router
   - Best signal quality
   - Most reliable connection

2. **Vote Percentage** (90% threshold)
   - Configured via `esp_mesh_set_vote_percentage(0.9)`
   - Prevents frequent root changes
   - Ensures stability

3. **Network Topology**
   - Tree topology (MESH_TOPO_TREE)
   - Single root, multiple layers
   - Max 6 connections per node

### Code Configuration

```c
// In mesh_init.c:
ESP_ERROR_CHECK(esp_mesh_set_topology(MESH_TOPO_TREE));
ESP_ERROR_CHECK(esp_mesh_set_root_healing_delay(10000));  // 10s
ESP_ERROR_CHECK(esp_mesh_allow_root_conflicts(true));     // Allow switching
ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(0.9));       // 90% threshold
```

## Identifying the Root Node

### Via Serial Logs

**Root node will show:**
```
I (xxxx) MESH_INIT: Mesh started
I (xxxx) IP_EVENT: Root got IP: 192.168.1.x
I (xxxx) MESH_INIT: Root toDS state: 1
I (xxxx) NODE_ROOT: Initializing MQTT client
I (xxxx) NODE_ROOT: MQTT connected
```

**Leaf nodes will show:**
```
I (xxxx) MESH_INIT: Mesh started
I (xxxx) MESH_INIT: Parent connected - Layer: 2
I (xxxx) MESH_INIT: Root address: 6C:C8:40:07:12:78
```

### Via NeoPixel LED (Switch Nodes)

- **Red**: Not connected to mesh
- **Yellow**: Mesh started but not fully connected
- **Green**: Fully connected (leaf or root)

### Via Variables

```c
// Check if this device is root
if (g_is_root) {
    ESP_LOGI(TAG, "I am the ROOT node");
} else {
    ESP_LOGI(TAG, "I am a LEAF node at layer %d", g_mesh_layer);
}
```

## Root Node Behavior

### WiFi Connection

Root node acts as a **bridge** between mesh and external network:

```
WiFi Router (192.168.1.1)
    ↓ (STA mode - gets IP via DHCP)
Root Node (192.168.1.x)
    ↓ (Mesh network)
Leaf Nodes (no external IP)
```

### MQTT Connection

Only the root node connects to MQTT:

```c
// ip_event_handler in mesh_init.c
if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    g_is_root = true;
    mqtt_init();  // Only root initializes MQTT
}
```

### Responsibilities

**Root Node:**
- ✅ Connects to WiFi router (STA mode)
- ✅ Gets IP address via DHCP
- ✅ Connects to MQTT broker
- ✅ Subscribes to MQTT command topics
- ✅ Forwards MQTT commands to mesh nodes
- ✅ Publishes mesh node status to MQTT
- ✅ Routes button presses to relay nodes
- ✅ Maintains device tracking table

**Leaf Nodes:**
- ❌ No direct WiFi router connection
- ❌ No IP address
- ❌ No MQTT connection
- ✅ Communicate via mesh only
- ✅ Send status to root via mesh
- ✅ Receive commands from root via mesh
- ✅ Report button presses to root
- ✅ Execute relay commands

## Root Failover and Healing

### Root Healing

If the root node fails or loses WiFi connection:

```
1. Root disconnection detected
2. Wait for healing delay (10 seconds)
3. New root election begins
4. Device with best WiFi signal becomes new root
5. New root gets IP via DHCP
6. New root initializes MQTT
7. Mesh network reconnects to new root
```

### Configuration

```c
// Root healing delay: 10 seconds
ESP_ERROR_CHECK(esp_mesh_set_root_healing_delay(10000));

// Allow root conflicts (enables failover)
ESP_ERROR_CHECK(esp_mesh_allow_root_conflicts(true));
```

### Events

```c
// Mesh events related to root
case MESH_EVENT_ROOT_ADDRESS:
    // Root node address announced
    
case MESH_EVENT_ROOT_FIXED:
    // Root node fixed/stable
    
case MESH_EVENT_ROOT_ASKED_YIELD:
    // Current root asked to yield (new root elected)
```

## Multiple Device Scenarios

### Scenario 1: 2 ESP32-C3 + 1 ESP32 Relay

**Question:** Which device becomes root?

**Answer:** The device with the **strongest WiFi router signal** (RSSI).

```
Device A (ESP32-C3): RSSI -45 dBm  ← Likely becomes root
Device B (ESP32-C3): RSSI -60 dBm
Device C (ESP32):    RSSI -55 dBm
```

**Why ESP32-C3 often becomes root:**
- Usually used as switch nodes
- Often placed in more accessible locations
- Better WiFi antenna positioning
- Closer to router

### Scenario 2: Forcing a Specific Root

**Not recommended**, but possible via:

1. **Physical placement**
   - Place desired root device closest to WiFi router
   - Ensure strong WiFi signal

2. **Root priority** (advanced)
   - Modify `esp_mesh_set_vote_percentage()`
   - Adjust RSSI thresholds
   - Not currently exposed in config

3. **Manual selection** (very advanced)
   - Use `esp_mesh_fix_root(true)` on specific device
   - Requires code modification
   - Prevents automatic failover

## Troubleshooting Root Election

### Problem: No Device Becomes Root

**Symptoms:**
```
I (xxxx) MESH_INIT: Mesh started
W (xxxx) MESH_INIT: WiFi STA disconnected
```
No device gets IP address.

**Causes:**
1. WiFi router SSID/password incorrect
2. WiFi router not reachable
3. All devices have weak signal

**Solutions:**
1. Check `CONFIG_WIFI_SSID` and `CONFIG_WIFI_PASSWORD` in menuconfig
2. Verify WiFi router is powered on and broadcasting
3. Move at least one device closer to router
4. Check router DHCP is enabled
5. Verify router is on 2.4GHz (ESP32 doesn't support 5GHz)

### Problem: Root Keeps Switching

**Symptoms:**
```
I (xxxx) MESH_INIT: Root asked to yield
I (xxxx) NODE_ROOT: MQTT disconnected
I (yyyy) IP_EVENT: Root got IP: 192.168.1.x
I (yyyy) NODE_ROOT: Initializing MQTT client
```
Root node changes frequently.

**Causes:**
1. Multiple devices have similar WiFi signal strength
2. WiFi interference or instability
3. Vote percentage too low

**Solutions:**
1. Increase vote percentage (default 0.9 is good)
2. Improve WiFi signal strength to one device
3. Reduce WiFi interference
4. Consider fixing root node (advanced)

### Problem: Wrong Device Becomes Root

**Symptoms:**
Relay board becomes root instead of switch.

**Impact:**
- Relay board may be harder to access
- Switch would be better positioned for WiFi

**Solutions:**
1. **Physical:** Move switch closer to WiFi router
2. **Physical:** Move relay away from router
3. **Configuration:** Accept any device as root (all work fine)
4. **Advanced:** Fix root on specific device (requires code change)

## Network Topology

### Single Root, Tree Structure

```
                WiFi Router
                     |
                 Root Node (Layer 1)
                   /   \
                  /     \
            Node A       Node B (Layer 2)
              /            \
          Node C          Node D (Layer 3)
```

### Layer Assignment

- **Layer 1:** Root node only
- **Layer 2:** Direct children of root
- **Layer 3:** Children of layer 2 nodes
- **Layer N:** Maximum depth depends on network size

### Maximum Topology

```c
mesh_cfg.mesh_ap.max_connection = 6;  // Max 6 children per node
```

With 3 devices (2 switches + 1 relay):
```
Root
 ├── Leaf 1
 └── Leaf 2
```

All leaf nodes connect directly to root (Layer 2).

## Monitoring Root Status

### Log Analysis

**Check serial logs for:**
```bash
# Root node indicators
grep "Root got IP" /path/to/log
grep "MQTT connected" /path/to/log

# Leaf node indicators  
grep "Parent connected" /path/to/log
grep "Layer:" /path/to/log
```

### MQTT Topics

When root publishes status:
```
/switch/status/{device_id}  - Switch status includes layer info
/relay/status/{device_id}   - Relay status includes layer info
```

Root node itself publishes:
```json
{
  "deviceId": 1074205304,
  "layer": 1,              ← Layer 1 = Root
  "isRoot": true,
  "meshConnected": true
}
```

Leaf nodes publish:
```json
{
  "deviceId": 1074207536,
  "layer": 2,              ← Layer 2+ = Leaf
  "isRoot": false,
  "meshConnected": true,
  "parentId": 1074205304   ← Connected to root
}
```

## Best Practices

### For Reliable Root Election

1. **WiFi Router Placement**
   - Central location
   - 2.4GHz band enabled
   - Strong signal throughout mesh area

2. **Device Placement**
   - At least one device with strong WiFi signal
   - Acceptable for any device to be root
   - No functional difference in root capabilities

3. **Network Configuration**
   - Correct WiFi SSID and password
   - Router DHCP enabled
   - MQTT broker configured and reachable

4. **Mesh Configuration**
   - Vote percentage: 0.9 (default, good)
   - Root healing delay: 10 seconds (default, good)
   - Allow root conflicts: true (enables failover)

### For Production Deployment

1. **Test all devices as root**
   - Each device should be able to become root
   - Verify MQTT works from any device
   - Test root failover

2. **Monitor root stability**
   - Track root switching events
   - Log root node identity
   - Alert on frequent changes

3. **Document root behavior**
   - Note which device typically becomes root
   - Document WiFi signal strengths
   - Plan for root failure scenarios

## Configuration Reference

### WiFi Settings

```bash
idf.py menuconfig
→ Domator Mesh Configuration
  → WiFi SSID: "your_wifi_name"
  → WiFi Password: "your_wifi_password"
```

### Mesh Settings

```bash
idf.py menuconfig
→ Domator Mesh Configuration
  → Mesh ID: "DMESH0" (6 characters)
```

### MQTT Settings (Root Only)

```bash
idf.py menuconfig
→ Domator Mesh Configuration
  → MQTT Broker URL: "mqtt://192.168.1.100"
  → MQTT Broker Port: 1883
  → MQTT Username: "domator"
  → MQTT Password: "domator"
```

## Summary

**Key Points:**
- ✅ Root election is automatic based on WiFi signal strength
- ✅ Any device can become root (no functional difference)
- ✅ Root connects to WiFi router and MQTT broker
- ✅ Leaf nodes communicate via mesh only
- ✅ Root failover is automatic with 10-second healing delay
- ✅ Identify root via logs: "Root got IP" and "MQTT connected"

**What You Control:**
- ✅ WiFi SSID and password
- ✅ MQTT broker configuration
- ✅ Device physical placement (affects signal strength)

**What Mesh Controls:**
- ✅ Root node selection
- ✅ Root failover
- ✅ Network topology
- ✅ Layer assignment

---

*For MQTT troubleshooting, see [MQTT_TROUBLESHOOTING.md](MQTT_TROUBLESHOOTING.md)*  
*For network issues, see [MESH_CRASH_SUMMARY.md](MESH_CRASH_SUMMARY.md)*  
*Last Updated: February 7, 2026*
