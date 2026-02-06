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

## Hardware Support

- ESP32 (original) - Root and Relay nodes
- ESP32-C3 (primary target for switch nodes)
- ESP32-S3

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

## License

Copyright (c) Domator Project
