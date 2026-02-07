# Domator Mesh IDF - ESP-WIFI-MESH Firmware

Unified ESP-IDF firmware for Domator mesh nodes using ESP-WIFI-MESH.

## Overview

This project replaces the previous 3 separate ESP-NOW Arduino firmwares (rootLightMesh, buttonLightMesh, relayLightMesh) with a single unified ESP-IDF firmware.

## Features

### Phase 1 - Root Node Functionality
- Full status reports to MQTT every 15 seconds
- Device ID generation from MAC address
- Firmware hash tracking
- Leaf node status forwarding to MQTT
- Mesh layer and peer count tracking

### Phase 2 - Switch Node Functionality  
- 7-button GPIO reading with debounce (250ms)
- Button press messages to root via mesh
- NeoPixel status LED with color-coded states:
  - Red: Not connected to mesh
  - Yellow: Mesh started but not connected
  - Green: Fully connected and operational
  - Cyan flash: Button press confirmation
  - Blue: OTA in progress
- Click counter tracking
- Status reports every 15 seconds

### Phase 3 - Relay Node Functionality
- Automatic board detection (8-relay vs 16-relay)
- 8-relay board: Direct GPIO control on pins 32, 33, 25, 26, 27, 14, 12, 13
- 16-relay board: 74HC595 shift register control (pins 14=SER, 13=SRCLK, 12=RCLK, 5=OE)
- Physical buttons: 8 buttons (GPIO 16, 17, 18, 19, 21, 22, 34, 35) toggle corresponding relays
- Relay commands from MQTT:
  - Toggle: Send "a" to toggle relay 0, "b" for relay 1, etc.
  - Set state: Send "a0" to turn relay 0 OFF, "a1" to turn ON
  - Sync: Send "S" or "sync" to request full state sync
- Relay state confirmation: Automatic state reporting to root after any change
- State sync: Responds to sync requests from root
- Status reports every 15 seconds with relay count

### Phase 4 - Root Routing Logic
- **Connection map parsing (#13)**: Configure button → relay mappings via MQTT JSON
- **Button type config (#14)**: Support toggle (type=0) and stateful (type=1) buttons
- **Button → relay routing (#15)**: Automatically route button presses to configured relay targets
- **MQTT → node forwarding (#16)**: Forward MQTT commands to mesh nodes with device-specific targeting
- **Relay state → MQTT (#17)**: Publish relay states to MQTT (already implemented)
- **Node status → MQTT (#18)**: Forward node status reports (already implemented)
- **MQTT config receive (#19)**: Receive and apply configuration via `/switch/cmd/root`
- **Device-specific targeting**: Commands to specific device IDs use direct routing (not broadcast)
  - MQTT topics with device ID: `/relay/cmd/1074207536`
  - Root maintains device_id → MAC address mapping
  - Direct sends when device known, broadcasts as fallback
  - See [DEVICE_TARGETING.md](DEVICE_TARGETING.md) for details

### Phase 5 - Reliability
- **Peer health tracking (#20)**: Track node connectivity and health metrics
- **OTA updates (#22)**: Firmware updates via esp_https_ota
- **OTA trigger via mesh (#23)**: Trigger OTA updates through mesh network
- **Root-loss reset (#25)**: Auto-reset nodes after 5 minutes of root disconnection
- **Heap health monitoring (#26)**: Monitor heap usage with low (40KB) and critical (20KB) thresholds

### Phase 6 - Gestures & Scenes
- **Button gesture state machine (#1)**: Single/double/long press detection
  - Single press: Quick press and release (< 800ms)
  - Double press: Two quick presses within 400ms window
  - Long press: Hold button for 800ms or more
- **Configurable gesture enable bitmask (#2)**: NVS-persisted gesture config per button
  - Bit 0: Single press enabled
  - Bit 1: Double press enabled
  - Bit 2: Long press enabled
- **Gesture char encoding (#3)**: Character encoding for gesture types
  - Single press: 'a'-'g' (buttons 0-6)
  - Double press: 'h'-'n' (buttons 0-6)
  - Long press: 'o'-'u' (buttons 0-6)
- **Root connections map extension (#4)**: Gesture characters routed through existing connection map
- **Explicit relay set commands (#5)**: "a0"/"a1" commands for scenes (already supported)
- **Scene mapping (#6)**: One gesture can trigger multiple relay actions via routing
- **NVS backup of gesture config (#7)**: Gesture config persists across reboots
- **End-to-end config delivery (#8)**: MQTT → Root → Mesh → Switch → NVS flow
- **Fallback logic (#9)**: Disabled gestures gracefully default to single press

#### Configuration Examples
**Connection Map:**
```json
{
  "type": "connections",
  "data": {
    "switchDeviceId": {
      "a": [["relayDeviceId", "a"], ["relayDeviceId2", "b"]],
      "b": [["relayDeviceId", "c"]]
    }
  }
}
```

**Button Types:**
```json
{
  "type": "button_types",
  "data": {
    "switchDeviceId": {
      "a": 0,
      "b": 1
    }
  }
}
```

**Gesture Configuration:**
```json
{
  "type": "gesture_config",
  "device_id": "switchDeviceId",
  "data": {
    "0": 7,
    "1": 3,
    "2": 1
  }
}
```
Note: Bitmask values: 1=single only, 3=single+double, 7=all enabled

**OTA Trigger:**
```json
{
  "type": "ota_trigger",
  "url": "https://example.com/firmware.bin"
}
```
Note: Firmware can be hosted on any HTTP/HTTPS server. See [OTA_FIRMWARE_HOSTING.md](OTA_FIRMWARE_HOSTING.md) for detailed hosting options.

## Hardware Support

- ESP32 (original) - Root and Relay nodes
- ESP32-C3 (primary target for switch nodes)
- ESP32-S3

**Important Platform Notes:**
- Relay boards (8-relay and 16-relay) are designed for **ESP32 (original)** only
- ESP32-C3 has limited GPIO availability (GPIOs 0-21)
- Physical buttons on relay boards use GPIOs 34, 35 which don't exist on ESP32-C3
- If relay firmware runs on ESP32-C3:
  - ✅ Relay outputs will work correctly
  - ⚠️ Physical buttons will be automatically disabled (unavailable pins skipped)
  - For full functionality, use ESP32 (original) for relay boards

## Pin Mapping

### Switch Node (ESP32-C3)
#### Buttons
- Button 0-6: GPIO 0, 1, 3, 4, 5, 6, 7

#### LED
- NeoPixel: GPIO 8

### Relay Node - 8-Relay Board (ESP32)
#### Relay Outputs
- Relay 0-7: GPIO 32, 33, 25, 26, 27, 14, 12, 13

#### Physical Buttons
- Button 0-7: GPIO 16, 17, 18, 19, 21, 22, 34, 35

#### Status
- Status LED: GPIO 23

### Relay Node - 16-Relay Board (ESP32)
#### Shift Register Control
- SER (Data): GPIO 14
- SRCLK (Clock): GPIO 13
- RCLK (Latch): GPIO 12
- OE (Output Enable): GPIO 5

#### Physical Buttons
- Button 0-7: GPIO 16, 17, 18, 19, 21, 22, 34, 35

## Partition Table

Dual 1408K OTA partitions for reliable firmware updates:
- ota_0: 1408K at 0x10000
- ota_1: 1408K at 0x170000

## MQTT Topics

### Published by Root
- `/switch/state/root` - Root status reports (JSON)
- `/switch/state/root` - Forwarded leaf status reports (JSON with parentId)
- `/switch/state/{deviceId}` - Button press events (single char 'a'-'g')
- `/relay/state/{deviceId}/{relay}` - Relay state confirmations (single char '0' or '1')

### Subscribed by Root
- `/switch/cmd/+` - Switch commands (e.g., `/switch/cmd/{deviceId}`)
- `/switch/cmd` - Broadcast switch commands
- `/relay/cmd/+` - Relay commands (e.g., `/relay/cmd/{deviceId}`)
- `/relay/cmd` - Broadcast relay commands

### Relay Commands
Commands sent to `/relay/cmd/{deviceId}` or `/relay/cmd`:
- `a` - Toggle relay 0
- `b` - Toggle relay 1
- ... (up to `p` for relay 15 on 16-relay boards)
- `a0` - Set relay 0 OFF
- `a1` - Set relay 0 ON
- `S` or `sync` - Request full state sync

## Building

### With PlatformIO
```bash
pio run -e esp32c3
pio run -t upload -e esp32c3
```

### With ESP-IDF
```bash
idf.py build
idf.py flash monitor
```

## Configuration

Configuration is done via `menuconfig` or by editing `sdkconfig.defaults`:

- `CONFIG_WIFI_SSID` - WiFi SSID for mesh network
- `CONFIG_WIFI_PASSWORD` - WiFi password
- `CONFIG_MESH_ID` - Mesh network ID (6 bytes)
- `CONFIG_MQTT_BROKER_URL` - MQTT broker URL
- `CONFIG_MQTT_BROKER_PORT` - MQTT broker port (default: 1883)
- `CONFIG_MQTT_USERNAME` - MQTT username
- `CONFIG_MQTT_PASSWORD` - MQTT password

## Project Structure

```
uc/buttonsMeshIDF/
├── CMakeLists.txt              # Top-level build config
├── platformio.ini              # PlatformIO config (3 environments)
├── partitions.csv              # Dual 1408K OTA partitions
├── sdkconfig.defaults          # Default SDK config
├── README.md                   # This file
└── src/
    ├── CMakeLists.txt          # Component build config
    ├── Kconfig.projbuild       # Configuration menu
    ├── idf_component.yml       # Managed components
    ├── domator_mesh.h          # Main header file
    ├── domator_mesh.c          # Main entry point and globals
    ├── mesh_init.c             # WiFi and mesh initialization
    ├── mesh_comm.c             # Mesh communication tasks
    ├── node_root.c             # Root node MQTT functionality
    ├── node_switch.c           # Switch node button and LED
    └── node_relay.c            # Relay node control
```

## Status Report Format

### Root Node Status
```json
{
  "deviceId": 12345678,
  "parentId": 12345678,
  "type": "root",
  "freeHeap": 123456,
  "uptime": 1234,
  "meshLayer": 1,
  "peerCount": 5,
  "firmware": "v1.0.0",
  "clicks": 0,
  "lowHeap": 0
}
```

### Leaf Node Status (Switch)
```json
{
  "deviceId": 87654321,
  "type": "switch",
  "freeHeap": 98765,
  "uptime": 5678,
  "firmware": "v1.0.0",
  "clicks": 42,
  "rssi": 0,
  "disconnects": 1,
  "lowHeap": 2
}
```

### Leaf Node Status (Relay)
```json
{
  "deviceId": 11223344,
  "type": "relay",
  "freeHeap": 87654,
  "uptime": 3456,
  "firmware": "v1.0.0",
  "clicks": 15,
  "rssi": 0,
  "disconnects": 0,
  "lowHeap": 1,
  "outputs": 8
}
```

After forwarding by root, `parentId` is added.

## Troubleshooting

### Hardware Misdetection - Relay Detected as Switch
If ESP32 relay board is detected as SWITCH (check logs for "Hardware detected as: SWITCH") and crashes with WDT reset, see [HARDWARE_DETECTION_FIX.md](HARDWARE_DETECTION_FIX.md) for:
- Auto-detection algorithm explanation
- NVS hardware type override procedure
- Manual configuration methods

**Quick fix:** Set hardware type in NVS: `nvs_set domator hardware_type u8 1` (0=switch, 1=relay_8, 2=relay_16)

### Mesh Crashes After "WiFi STA started"
If device resets immediately after WiFi STA starts with incomplete mesh log `I (xxx) mesh:`, see [MESH_INIT_CRASH_FIX.md](MESH_INIT_CRASH_FIX.md) for:
- Memory exhaustion diagnosis
- WiFi buffer configuration fixes
- Heap monitoring techniques

**Quick fix:** Use provided `sdkconfig.defaults` which reduces WiFi buffers to prevent memory exhaustion.

### Relay Board Crashes on Startup (Initialization Order)
If relay board resets during mesh initialization with message like `I (xxx) mesh:`, see [RELAY_CRASH_FIX.md](RELAY_CRASH_FIX.md) for:
- Root cause explanation
- Verification steps
- Advanced debugging

**Quick fix:** Ensure firmware version ≥ v1.1.0 which includes initialization order fix.

### Device-Specific Targeting
Commands to specific devices use direct routing instead of broadcast. For details on how device targeting works, see [DEVICE_TARGETING.md](DEVICE_TARGETING.md).

**Usage:** Include device ID in MQTT topic:
```bash
# Target specific relay
mosquitto_pub -t "/relay/cmd/1074207536" -m "a1"

# Broadcast to all relays
mosquitto_pub -t "/relay/cmd" -m "S"
```

### Other Issues
- OTA firmware hosting: See [OTA_FIRMWARE_HOSTING.md](OTA_FIRMWARE_HOSTING.md)
- Phase 5 & 6 features: See [PHASE5_6_GUIDE.md](PHASE5_6_GUIDE.md)
- Quick start: See [QUICKSTART.md](QUICKSTART.md)

## License

Copyright (c) Domator Project
