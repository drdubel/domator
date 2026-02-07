# Root Transitions and MQTT Behavior

## Quick Answer

**Q: "All devices have same MQTT credentials - is there a possibility that 2 or 3 devices think they are root and connect to MQTT?"**

**A: NO** - After the fix implemented in this firmware:

- ✅ **Only ONE device is root** at any time (ESP-WIFI-MESH protocol guarantee)
- ✅ **Each device has unique MQTT client ID** (format: `domator_<device_id>`)
- ✅ **Old root cleanly disconnects** MQTT when losing root status
- ✅ **New root connects seamlessly** with its own unique ID
- ✅ **No duplicate client ID conflicts** at the MQTT broker

### Your Issue (Before Fix)

**Problem:** "One device connects to MQTT at first and then can't connect"

**Root cause:**
1. All devices used default/same MQTT client ID
2. Old root kept MQTT connected after losing root status
3. New root tried to connect with duplicate client ID
4. MQTT broker rejected duplicate connection
5. Result: MQTT broken during root transitions

**After fix:**
- Each device has unique ID: `domator_1074205304`, `domator_1074207536`, etc.
- Old root gets `MESH_EVENT_ROOT_LOST` → calls `mqtt_cleanup()`
- MQTT cleanly disconnected before new root connects
- New root connects with its own unique ID
- System continues functioning during transitions

---

## Understanding Root Election

### How ESP-WIFI-MESH Selects Root

ESP-WIFI-MESH automatically elects **ONE root node** based on:

1. **WiFi Signal Strength (RSSI)** - Primary factor
   - Device with strongest signal to WiFi router
   - Measured continuously

2. **Hop Count** - Secondary factor
   - Distance from WiFi router in mesh hops
   - Fewer hops preferred

3. **Vote Percentage** - Threshold: 90%
   - Nodes vote for root candidate
   - Must reach 90% agreement

4. **Root Score** - Calculated metric
   - Combination of RSSI, hops, and other factors
   - Highest score wins

### Election Timing

```
T+0s:    Mesh network starts
T+0-5s:  Devices discover each other
T+5-10s: Root election process
T+10s:   Root selected and fixed
```

**Typical election time:** ~10 seconds after mesh starts

### Single Root Guarantee

ESP-WIFI-MESH protocol **guarantees only ONE root** exists at any time:
- After election completes (usually 10s after start)
- During normal operation
- During root healing (with 10s transition)

**Brief exceptions (temporary):**
- During initial election (~10s at startup)
- During root healing transition (~10s when root changes)

---

## Root Healing (Root Transitions)

### What is Root Healing?

Root healing is the process where the mesh network **automatically changes** the root node when:

1. **Current root WiFi signal degrades**
2. **Current root device fails or powers off**
3. **Better root candidate appears** (stronger WiFi signal)
4. **Current root explicitly yields** (rare)

### Healing Delay

When root change is needed:
```
T+0s:   Trigger detected (e.g., weak WiFi signal)
T+0-10s: Evaluation period (is change really needed?)
T+10s:   New root elected
T+10s:   MESH_EVENT_ROOT_LOST sent to old root
T+10s:   MESH_EVENT_ROOT_FIXED sent to new root
```

**Default healing delay:** 10 seconds

### Why Healing Delay Exists

- Prevents rapid switching (WiFi signal fluctuations)
- Ensures stable root before committing
- Gives current root chance to recover
- Reduces network disruption

---

## MQTT Behavior During Root Transitions

### Before Fix (BROKEN)

```
Time 0:  Device A = Root, MQTT connected (default client ID)
Time 1:  Device A WiFi signal degrades
Time 10: Device B elected as new root
Time 10: Device A: Still has MQTT connected (no cleanup)
Time 11: Device B: Gets IP, tries to connect MQTT
Time 11: MQTT Broker: Rejects Device B (duplicate client ID)
Time 12: Device A: MQTT still connected but shouldn't be root
Time 12: Device B: Can't connect MQTT
Result:  MQTT broken, system non-functional
```

### After Fix (WORKING)

```
Time 0:  Device A = Root, MQTT connected (domator_1074205304)
Time 1:  Device A WiFi signal degrades
Time 10: Device B elected as new root
Time 10: Device A: MESH_EVENT_ROOT_LOST → mqtt_cleanup()
Time 10: Device A: Disconnects MQTT cleanly
Time 11: Device B: Gets IP → mqtt_init()
Time 11: Device B: Connects MQTT (domator_1074207536)
Time 12: Device B: MQTT connected, system operational
Result:  Clean transition, ~12 second downtime, system continues
```

### MQTT Client ID Strategy

Each device has unique client ID based on device ID:

| Device ID | MAC Address | MQTT Client ID |
|-----------|-------------|----------------|
| 1074205304 | 6C:C8:40:07:12:78 | domator_1074205304 |
| 1074207536 | 6C:C8:40:07:13:10 | domator_1074207536 |
| 1074208123 | 6C:C8:40:07:14:0B | domator_1074208123 |

**Result:**
- No conflicts at MQTT broker
- Each device can connect when it becomes root
- Broker can distinguish between devices
- Clean disconnection messages

---

## Step-by-Step Transition Flow

### Scenario: Device A (Root) → Device B (New Root)

#### Step 1: Initial State
```
Device A: Root, WiFi RSSI = -40 dBm, MQTT connected
Device B: Leaf, WiFi RSSI = -50 dBm, No MQTT
Device C: Leaf, WiFi RSSI = -60 dBm, No MQTT
```

#### Step 2: Trigger (WiFi Degradation)
```
Device A: WiFi RSSI drops to -70 dBm (weak signal)
Device B: WiFi RSSI improves to -35 dBm (strong signal)
Device C: WiFi RSSI = -60 dBm (no change)

Mesh: Detects Device A is no longer best root candidate
```

#### Step 3: Evaluation Period (0-10 seconds)
```
Mesh: Monitors Device A signal
Mesh: Confirms Device A is consistently weak
Mesh: Identifies Device B as better candidate
Mesh: Waits 10 seconds to ensure stable
```

#### Step 4: Root Change (T+10s)
```
Mesh: Elects Device B as new root
Mesh: Sends MESH_EVENT_ROOT_LOST to Device A
Mesh: Sends MESH_EVENT_ROOT_FIXED to Device B
```

#### Step 5: Old Root Cleanup (T+10s)
```
Device A: Receives MESH_EVENT_ROOT_LOST
Device A: Sets g_is_root = false
Device A: Calls mqtt_cleanup()
Device A: Publishes disconnection status
Device A: Stops MQTT client
Device A: Destroys MQTT client
Device A: Now a leaf node
```

**Log output (Device A):**
```
W (120000) MESH_INIT: Root lost - this device is no longer root
I (120001) NODE_ROOT: Cleaning up MQTT client (no longer root)
I (120002) NODE_ROOT: Publishing disconnection status
I (120010) NODE_ROOT: MQTT client cleaned up
```

#### Step 6: New Root Initialization (T+11s)
```
Device B: Connects to WiFi router as STA
Device B: Gets IP address via DHCP
Device B: Receives IP_EVENT_STA_GOT_IP
Device B: Sets g_is_root = true
Device B: Calls mqtt_init()
Device B: Creates MQTT client with unique ID: domator_1074207536
Device B: Starts MQTT client
```

**Log output (Device B):**
```
I (120500) MESH_INIT: Root got IP: 192.168.1.45
I (120501) NODE_ROOT: Initializing MQTT client
I (120502) NODE_ROOT: Using MQTT client ID: domator_1074207536
I (120505) NODE_ROOT: MQTT client started
```

#### Step 7: MQTT Connection (T+12s)
```
Device B: Connects to MQTT broker
Device B: Broker accepts connection (unique client ID)
Device B: Subscribes to command topics
Device B: Publishes connection status
Device B: System fully operational
```

**Log output (Device B):**
```
I (121200) NODE_ROOT: MQTT connected
I (121201) NODE_ROOT: Publishing connection status
I (121202) NODE_ROOT: Subscribed to /switch/cmd/+
I (121203) NODE_ROOT: Subscribed to /relay/cmd/+
```

#### Step 8: Final State (T+12s+)
```
Device A: Leaf node, No MQTT
Device B: Root node, MQTT connected (domator_1074207536)
Device C: Leaf node, No MQTT

System: Fully operational, commands work via Device B
```

### Total Transition Time

```
Trigger → New root operational: ~12 seconds
├─ Evaluation delay: 10s
├─ Old root cleanup: 1s
└─ New root MQTT connect: 1s
```

---

## Multiple Device Scenarios

### Scenario 1: 2 Devices

**Setup:**
- Device A: 2m from router
- Device B: 10m from router

**Initial:**
```
Device A: RSSI = -35 dBm → Root + MQTT (domator_1074205304)
Device B: RSSI = -55 dBm → Leaf, no MQTT
```

**After Device A powers off:**
```
Device B: Becomes root, connects MQTT (domator_1074207536)
Device A: Offline
```

**When Device A comes back online:**
```
Device A: RSSI = -35 dBm (stronger)
Device B: RSSI = -55 dBm (weaker)
Result:  Device A becomes root again, MQTT transitions
         Device B: Cleanup MQTT → Leaf
         Device A: Init MQTT → Root
```

### Scenario 2: 3 Devices (User's Case)

**Setup:**
- 2 ESP32-C3 (switches)
- 1 ESP32 (relay board)
- All near router

**Root selection:** Whichever has strongest WiFi signal at the moment

**Example:**
```
ESP32-C3 #1: RSSI = -40 dBm → Root + MQTT (domator_1074205304)
ESP32-C3 #2: RSSI = -45 dBm → Leaf
ESP32 Relay: RSSI = -50 dBm → Leaf
```

**If ESP32-C3 #1 WiFi weakens:**
```
ESP32-C3 #1: RSSI = -60 dBm → Loses root, cleanup MQTT → Leaf
ESP32-C3 #2: RSSI = -45 dBm → Becomes root, init MQTT
ESP32 Relay: RSSI = -50 dBm → Remains leaf
```

**Note:** Any of the 3 devices can be root - functionality is identical.

### Scenario 3: Large Deployment (10+ Devices)

**Principles:**
- Only 1 device is root (controls MQTT)
- All others are leaf nodes (relay through mesh)
- Root is always device with best WiFi to router
- Root can change as devices move or signal varies
- Each transition takes ~12 seconds
- System remains operational during transitions

---

## Understanding the Logs

### Station Join Message

User's log:
```
I (169904) wifi:station: e8:f6:0a:38:cf:a4 join, AID=1, bgn, 40U
```

**Meaning:**
- A device (MAC: e8:f6:0a:38:cf:a4) is joining the mesh network
- "station" = leaf node connecting to root's soft AP
- "AID=1" = Association ID 1 (first device to join)
- "bgn" = 802.11b/g/n mode
- "40U" = 40 MHz upper channel

**This is NORMAL:**
- Shows mesh network is forming
- Leaf nodes connecting to root
- Not an error or problem
- Not related to multiple roots (only root has station connections)

### MQTT Connection Logs

**Successful connection:**
```
I (7245) MESH_INIT: Root got IP: 192.168.1.45
I (7246) NODE_ROOT: Initializing MQTT client
I (7251) NODE_ROOT: Using MQTT client ID: domator_1074205304
I (7252) NODE_ROOT: MQTT client started
I (8123) NODE_ROOT: MQTT connected
I (8124) NODE_ROOT: Publishing connection status
```

**Connection failure (duplicate client ID - legacy):**
```
I (7251) NODE_ROOT: MQTT client started
E (8123) NODE_ROOT: MQTT error
W (8124) NODE_ROOT: MQTT disconnected
E (10123) NODE_ROOT: MQTT error
W (10124) NODE_ROOT: MQTT disconnected
```
*Note: This should not occur with the unique client ID fix*

### Root Transition Logs

**Old root losing status:**
```
W (120000) MESH_INIT: Root lost - this device is no longer root
I (120001) NODE_ROOT: Cleaning up MQTT client (no longer root)
I (120010) NODE_ROOT: MQTT client cleaned up
```

**New root gaining status:**
```
I (120500) esp_netif_handlers: sta ip: 192.168.1.45, mask: 255.255.255.0, gw: 192.168.1.1
I (120500) MESH_INIT: Root got IP: 192.168.1.45
I (120501) NODE_ROOT: Initializing MQTT client
I (120502) NODE_ROOT: Using MQTT client ID: domator_1074207536
I (121200) NODE_ROOT: MQTT connected
```

---

## Troubleshooting

### Problem: Frequent Root Changes

**Symptoms:**
- Root status changes every few minutes
- Frequent "Root lost" messages in logs
- MQTT repeatedly disconnecting/reconnecting

**Causes:**
1. **Borderline WiFi signals** - Multiple devices with similar RSSI
2. **WiFi interference** - Other networks, microwaves, etc.
3. **Device placement** - Devices equidistant from router
4. **Router issues** - Weak signal, channel congestion

**Solutions:**
1. **Improve placement** - Move one device significantly closer to router
2. **Fix WiFi** - Change router channel, improve signal
3. **Reduce interference** - Move away from obstacles, other devices
4. **Force root** (not recommended) - Set root with esp_mesh_set_root()

### Problem: MQTT Doesn't Reconnect After Root Change

**Symptoms:**
- Root changes but MQTT stays disconnected
- No "MQTT connected" log after root change
- Commands don't work after transition

**Possible Causes:**
1. **MQTT broker unreachable** - Network issue, broker down
2. **Wrong broker address** - Device can't find broker
3. **Firewall blocking** - New IP blocked at broker
4. **Client ID conflict** (legacy) - Old firmware without fix

**Solutions:**
1. **Verify broker** - Check broker is running and reachable
2. **Check network** - Ensure root has internet/LAN access
3. **Update firmware** - Ensure unique client ID fix applied
4. **Check logs** - Look for specific MQTT error messages

### Problem: "Duplicate client ID" Error at Broker (Legacy)

**Symptoms:**
- MQTT broker logs show "client already connected"
- Second device can't connect
- Connection repeatedly rejected

**Cause:**
- Old firmware without unique client ID fix
- Multiple devices using same client ID

**Solution:**
- **Update firmware** - Must use version with unique client ID support
- Each device will get unique ID: domator_<device_id>
- No manual configuration needed

### Problem: Which Device is Root?

**Methods to identify:**

1. **Serial logs:**
```bash
# Root device logs:
"Root got IP: 192.168.1.45"
"MQTT connected"

# Leaf device logs:
"Mesh connected to parent"
"Mesh layer: 2" (or higher)
```

2. **MQTT monitoring:**
```bash
# Subscribe to connection status
mosquitto_sub -t "/status/root/connection" -v

# Message includes device_id of root
{"status":"connected","device_id":1074205304,...}
```

3. **Device LED** (if implemented):
- Root: Usually green or different pattern
- Leaf: Usually blue or different pattern

---

## Monitoring Root Status

### Via Serial Logs

**Root device:**
```
I (7245) MESH_INIT: Root got IP: 192.168.1.45
I (8123) NODE_ROOT: MQTT connected
```

**Leaf device:**
```
I (6234) MESH_INIT: Mesh connected to parent
I (6235) MESH_INIT: Mesh layer: 2
```

### Via MQTT Connection Status

**Subscribe to status topic:**
```bash
mosquitto_sub -t "/status/root/connection" -v
```

**Connection message:**
```json
{
  "status": "connected",
  "device_id": 1074205304,
  "timestamp": 1707308070,
  "firmware": "57462b5",
  "ip": "192.168.1.45",
  "mesh_layer": 1
}
```

### Via Home Assistant

**Binary sensor:**
```yaml
mqtt:
  binary_sensor:
    - name: "Mesh Network Online"
      state_topic: "/status/root/connection"
      value_template: "{{ value_json.status == 'connected' }}"
      device_class: connectivity
      
  sensor:
    - name: "Mesh Root Device"
      state_topic: "/status/root/connection"
      value_template: "{{ value_json.device_id }}"
      
    - name: "Mesh Root IP"
      state_topic: "/status/root/connection"
      value_template: "{{ value_json.ip }}"
```

---

## Best Practices

### Optimal Device Placement

1. **One device near router** - Ensures stable root
2. **Others spread out** - Cover desired area
3. **Avoid equidistant** - Prevents frequent root changes
4. **Consider obstacles** - Walls, metal, water affect signal

### WiFi Configuration

1. **Use 2.4 GHz** - ESP32 doesn't support 5 GHz
2. **Fixed channel** - Prevents channel switching
3. **Good router placement** - Central, elevated
4. **Minimize interference** - Other devices, networks

### Preventing Unnecessary Transitions

1. **Stable WiFi** - Strong, consistent signal to router
2. **One clear winner** - One device significantly closer
3. **Avoid movement** - Keep devices stationary
4. **Monitor signal** - Check RSSI values are stable

### Production Deployment

1. **Test transitions** - Power off root, verify failover
2. **Monitor MQTT** - Watch connection status
3. **Log aggregation** - Collect logs from all devices
4. **Alerting** - Notify on frequent root changes
5. **Documentation** - Note which device is usually root

---

## Summary

### Key Points

- ✅ **Only ONE root at a time** - ESP-WIFI-MESH protocol guarantee
- ✅ **Unique MQTT client IDs** - No conflicts (domator_<device_id>)
- ✅ **Clean transitions** - Old root disconnects, new root connects
- ✅ **~12 second failover** - System continues with brief interruption
- ✅ **Automatic recovery** - No manual intervention needed
- ✅ **Any device can be root** - Functionality identical

### Answer to Original Question

**"Is it possible that 2-3 devices think they are root and connect to MQTT?"**

**NO:**
- ESP-WIFI-MESH ensures single root
- Each device has unique MQTT client ID
- Old root cleanly disconnects before new root connects
- MQTT broker accepts only current root's connection
- System designed to prevent this scenario

Your issue was the duplicate client ID problem, which has been fixed. The system now handles root transitions gracefully with no MQTT conflicts.

---

## See Also

- [ROOT_ELECTION.md](ROOT_ELECTION.md) - Detailed root election process
- [MQTT_TROUBLESHOOTING.md](MQTT_TROUBLESHOOTING.md) - MQTT setup and issues
- [MQTT_STATUS_MESSAGES.md](MQTT_STATUS_MESSAGES.md) - Connection status monitoring
- [MESH_TROUBLESHOOTING.md](MESH_TROUBLESHOOTING.md) - General mesh issues
