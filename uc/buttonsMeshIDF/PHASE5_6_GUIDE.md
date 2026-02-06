# Phase 5 & 6 Implementation Guide

## Overview

This document describes the implementation of Phase 5 (Reliability) and Phase 6 (Gestures & Scenes) features for the buttonsMeshIDF project.

---

## Phase 6 - Gestures & Scenes

### Gesture State Machine

The gesture detection system uses a sophisticated state machine to detect three types of button interactions:

1. **Single Press**: Quick press and release (< 800ms)
2. **Double Press**: Two quick presses within 400ms window
3. **Long Press**: Hold button for 800ms or more

#### State Machine Flow

```
Button Released → Wait for press
    ↓
Button Pressed → Start timer
    ↓
    ├─ Released < 800ms → Wait for possible double press
    │   ↓
    │   ├─ Pressed within 400ms → DOUBLE PRESS detected
    │   └─ Timeout 400ms → SINGLE PRESS detected
    │
    └─ Held ≥ 800ms → LONG PRESS detected on release
```

#### Gesture Character Encoding

Gestures are encoded as single characters for transmission:

- **Single Press**: `'a'` through `'g'` (buttons 0-6)
- **Double Press**: `'h'` through `'n'` (buttons 0-6)
- **Long Press**: `'o'` through `'u'` (buttons 0-6)

Example:
- Button 0 single press: `'a'`
- Button 0 double press: `'h'`
- Button 0 long press: `'o'`
- Button 2 double press: `'j'`

### Gesture Configuration

Each button has a configurable enable bitmask stored in NVS:

- **Bit 0 (0x01)**: Single press enabled
- **Bit 1 (0x02)**: Double press enabled
- **Bit 2 (0x04)**: Long press enabled

Common configurations:
- `0x01` (1): Single press only
- `0x03` (3): Single + Double press
- `0x05` (5): Single + Long press
- `0x07` (7): All gestures enabled (default)

### NVS Storage

Gesture configurations are stored in NVS with keys:
- `gesture_0` through `gesture_6` for buttons 0-6

The configuration persists across reboots and can be updated via MQTT.

### Configuration Delivery

Configuration flows from MQTT to the device:

```
MQTT → Root Node → Mesh Network → Switch Node → NVS Storage
```

1. Send JSON config to `/switch/cmd/root`
2. Root parses and forwards to target switch via mesh
3. Switch receives config and applies it
4. Config saved to NVS for persistence

#### Example Configuration Message

```json
{
  "type": "gesture_config",
  "device_id": "12345678",
  "data": {
    "0": 7,  // Button 0: all gestures enabled
    "1": 3,  // Button 1: single + double only
    "2": 1,  // Button 2: single press only
    "3": 7,  // Button 3: all gestures
    "4": 7,  // Button 4: all gestures
    "5": 5,  // Button 5: single + long only
    "6": 7   // Button 6: all gestures
  }
}
```

### Fallback Behavior

When a gesture is disabled:
- The gesture is detected but not sent
- Instead, a single press is sent as fallback
- This ensures buttons remain functional even with disabled gestures

Example:
- Button configured with bitmask `0x01` (single only)
- User performs double press
- System detects double press but gesture disabled
- Sends single press instead

### Root Routing Extension

The root routing system automatically handles all gesture characters:

1. Gesture character received (e.g., `'h'` for button 0 double press)
2. Mapped to base button index (0)
3. Routing table consulted for button 0
4. Commands sent to all configured targets

This means the same routing configuration works for all gesture types.

---

## Phase 5 - Reliability

### Peer Health Tracking

The root node tracks health of all connected devices:

**Tracked Metrics:**
- Device ID
- Last seen timestamp
- Disconnect count
- Last RSSI (placeholder, mesh doesn't provide this)
- Alive status

**Health Check Interval:** 30 seconds

**Timeout Threshold:** 60 seconds (configurable)

When a peer times out:
- Marked as not alive
- Disconnect count incremented
- Warning logged

### OTA Updates

Firmware updates via HTTPS using `esp_https_ota` component.

#### OTA Flow

```
MQTT Trigger → Root Node → Mesh Broadcast → All Nodes → Download & Install
```

#### OTA Configuration Message

```json
{
  "type": "ota_trigger",
  "url": "https://example.com/firmware.bin"
}
```

**Features:**
- HTTPS support with certificate bundle
- Mesh-based trigger propagation
- Automatic restart after successful update
- LED indicator (blue) during OTA

**Safety:**
- Dual OTA partition support (ota_0 and ota_1)
- Automatic rollback on boot failure
- Verification of downloaded firmware

### Root Loss Detection & Reset

Non-root nodes monitor connection to root:

**Monitoring:**
- Check every 10 seconds
- Track last successful root contact
- Timeout: 5 minutes (configurable)

**On Timeout:**
- Log error with disconnect duration
- Track disconnect in statistics
- Automatic device restart

This prevents nodes from being stuck in a disconnected state.

### Heap Health Monitoring

Continuous monitoring of heap memory:

**Check Interval:** 5 seconds

**Thresholds:**
- Low Heap: 40,000 bytes
- Critical Heap: 20,000 bytes

**Actions:**
- Warning logged (max once per minute)
- Statistics updated
- Low/critical event counters incremented

**Statistics Tracked:**
- `low_heap_events`: Count of low heap warnings
- `critical_heap_events`: Count of critical heap warnings

---

## Configuration Reference

### Gesture Config

**Topic:** `/switch/cmd/root`

**Format:**
```json
{
  "type": "gesture_config",
  "device_id": "<device_id>",
  "data": {
    "<button_index>": <bitmask>
  }
}
```

**Parameters:**
- `device_id`: Target switch device ID (string or number)
- `button_index`: Button index (0-6)
- `bitmask`: Gesture enable bitmask (0-7)

### Connection Map

**Topic:** `/switch/cmd/root`

**Format:**
```json
{
  "type": "connections",
  "data": {
    "<device_id>": {
      "<button_char>": [
        ["<target_device_id>", "<relay_command>"],
        ...
      ]
    }
  }
}
```

Works for all gesture characters (a-g, h-n, o-u).

### Button Types

**Topic:** `/switch/cmd/root`

**Format:**
```json
{
  "type": "button_types",
  "data": {
    "<device_id>": {
      "<button_char>": <type>
    }
  }
}
```

**Types:**
- `0`: Toggle (command sent as-is)
- `1`: Stateful (state appended to command)

### OTA Trigger

**Topic:** `/switch/cmd/root`

**Format:**
```json
{
  "type": "ota_trigger",
  "url": "<firmware_url>"
}
```

Broadcasts to all mesh nodes for simultaneous update.

---

## Testing Guide

### Test Gesture Detection

1. Configure gesture enables via MQTT
2. Press button with different patterns:
   - Quick press: Single
   - Two quick presses: Double
   - Hold for 1 second: Long
3. Check MQTT for correct character:
   - Single: a-g
   - Double: h-n
   - Long: o-u
4. Verify cyan LED flash on each detection

### Test Gesture Configuration

1. Send gesture config with specific bitmask
2. Check device logs for "Gesture configuration saved to NVS"
3. Try disabled gesture (e.g., double with bitmask 0x01)
4. Verify fallback to single press
5. Restart device
6. Verify config persisted (check logs on startup)

### Test OTA

1. Build new firmware with version change
2. Host on HTTPS server
3. Send OTA trigger JSON
4. Check device logs for "Starting HTTPS OTA update"
5. Verify LED turns blue
6. Wait for update and automatic restart
7. Check new firmware version in logs

### Test Peer Health

1. Configure root node
2. Add switch/relay nodes to mesh
3. Check root logs for "Added peer X to health tracking"
4. Wait 30+ seconds
5. Check logs for "Peer health: X/Y peers alive"
6. Disconnect a node
7. After 60 seconds, verify "Peer X timeout" logged

### Test Root Loss Reset

1. Configure leaf node (switch or relay)
2. Connect to mesh (verify green LED)
3. Power off root node
4. Wait 5+ minutes
5. Verify leaf node restarts automatically
6. Check logs for "Root lost for X ms, resetting device"

### Test Heap Monitoring

1. Monitor device logs
2. Look for periodic free heap reports
3. If heap drops below thresholds:
   - Low: "Low heap detected: X bytes free"
   - Critical: "CRITICAL heap level: X bytes free"

---

## Troubleshooting

### Gestures Not Working

1. Check gesture config: `gesture_0` through `gesture_6` in NVS
2. Verify bitmask includes desired gesture (1=single, 2=double, 4=long)
3. Check button timing:
   - Double: < 400ms between presses
   - Long: ≥ 800ms hold time
4. Verify mesh connection (green LED)

### Configuration Not Persisting

1. Check NVS initialization in logs
2. Verify "Gesture configuration saved to NVS" appears
3. Check for NVS errors (may need `nvs_flash_erase()`)
4. Ensure device has flash partition for NVS

### OTA Fails

1. Check HTTPS certificate (must be valid)
2. Verify URL is accessible from device
3. Check firmware size (must fit in OTA partition)
4. Verify sufficient free heap before OTA
5. Check logs for specific error code

### Root Loss Not Triggering Reset

1. Verify timeout value (5 minutes default)
2. Check `g_last_root_contact` is being updated
3. Ensure task is running (check task creation)
4. Verify mesh connection state tracking

---

## Performance Characteristics

| Feature | Timing/Memory | Notes |
|---------|--------------|-------|
| Gesture detection | ~10-50ms overhead | Per button press |
| Double press window | 400ms | Configurable |
| Long press threshold | 800ms | Configurable |
| Debounce time | 250ms | Fixed |
| Root loss timeout | 5 minutes | Configurable |
| Peer health check | 30 seconds | Configurable |
| Heap monitoring | 5 seconds | Fixed |
| NVS config size | ~7 bytes | Per button |
| OTA partition | 1408KB | Dual partitions |

---

## Known Limitations

1. **Direct Addressing**: Commands broadcast to all nodes; nodes filter by device ID
2. **RSSI Tracking**: ESP-WIFI-MESH doesn't provide per-message RSSI
3. **Config Size**: Mesh messages limited to 200 bytes
4. **OTA Simultaneous**: All nodes attempt OTA simultaneously (may overwhelm server)
5. **7 Buttons Only**: Gesture system supports 7 buttons (ESP32-C3 limitation)

---

## Future Enhancements

1. **Advanced Gestures**: Triple press, hold-and-release patterns
2. **Gesture Macros**: Sequence of button presses triggers action
3. **Direct Addressing**: MAC-based routing for better performance
4. **Staged OTA**: Update nodes in groups to reduce server load
5. **Web Configuration**: Web UI for gesture and routing config
6. **RSSI Integration**: Add custom RSSI tracking mechanism
7. **Gesture Analytics**: Track most-used gestures for optimization

---

*Implementation Date: February 6, 2026*
*ESP-IDF Version: 5.4.0*
*Project: drdubel/domator*
