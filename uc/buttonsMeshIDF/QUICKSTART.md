# Phase 5 & 6 Quick Start Guide

## For Developers Testing Phase 5 & 6 Features

This guide helps you quickly test the new gesture and reliability features.

---

## Prerequisites

- ESP32 (for root/relay) or ESP32-C3 (for switch)
- ESP-IDF 5.4.0 or PlatformIO
- MQTT broker (Mosquitto recommended)
- MQTT client (mosquitto_pub/sub or MQTT Explorer)

---

## Quick Setup (5 Minutes)

### 1. Configure WiFi & MQTT

Edit `sdkconfig.defaults`:
```ini
CONFIG_WIFI_SSID="YourWiFi"
CONFIG_WIFI_PASSWORD="YourPassword"
CONFIG_MQTT_BROKER_URL="mqtt://192.168.1.100"
CONFIG_MQTT_BROKER_PORT=1883
```

### 2. Build & Flash

```bash
cd uc/buttonsMeshIDF

# ESP-IDF
idf.py build flash monitor

# OR PlatformIO
pio run -e esp32c3 -t upload -t monitor
```

### 3. Verify Basic Operation

Watch for in logs:
```
I (xxx) DOMATOR_MESH: Device ID: 12345678
I (xxx) NODE_SWITCH: Gesture configuration loaded
I (xxx) MESH_INIT: Mesh started
```

LED should show:
- Red → Yellow → Green (connected)

---

## Test Gestures (2 Minutes)

### Subscribe to Button Events

```bash
mosquitto_sub -h localhost -t "/switch/state/#" -v
```

### Test Each Gesture Type

1. **Single Press** (press & release quickly):
   - Press button 0
   - See: `/switch/state/12345678` → `a`

2. **Double Press** (two quick presses):
   - Press button 0 twice within 400ms
   - See: `/switch/state/12345678` → `h`

3. **Long Press** (hold 1 second):
   - Hold button 0 for 1+ second
   - See: `/switch/state/12345678` → `o`

### Verify LED Feedback

- Each gesture should flash LED cyan briefly

---

## Configure Gestures (1 Minute)

### Disable Double Press on Button 0

```bash
mosquitto_pub -h localhost -t "/switch/cmd/root" -m '{
  "type": "gesture_config",
  "device_id": "12345678",
  "data": {
    "0": 5
  }
}'
```

Bitmask `5` = `0b101` = single + long (no double)

### Test Fallback

- Try double press on button 0
- Should send single press `a` instead of `h`

### Verify Persistence

```bash
# Restart device
# Try gestures again
# Config should persist!
```

---

## Setup a Scene (2 Minutes)

### Configure Routing

Assuming you have relay node with ID 87654321:

```bash
mosquitto_pub -h localhost -t "/switch/cmd/root" -m '{
  "type": "connections",
  "data": {
    "12345678": {
      "h": [
        ["87654321", "a1"],
        ["87654321", "b1"],
        ["87654321", "c0"]
      ]
    }
  }
}'
```

### Test Scene

- Double press button 0 on switch
- Relay should turn on relays a & b
- Relay should turn off relay c

---

## Test OTA (5 Minutes)

### Prepare Firmware

1. Modify firmware (e.g., change log message)
2. Build: `idf.py build` or `pio run`
3. Locate binary: `build/buttonsMeshIDF.bin`

### Host Firmware

```bash
# Simple Python HTTPS server
cd build
python3 -m http.server 8000

# Note: For HTTPS, use nginx or proper server
# For testing, you can use HTTP (less secure)
```

### Trigger OTA

```bash
mosquitto_pub -h localhost -t "/switch/cmd/root" -m '{
  "type": "ota_trigger",
  "url": "http://192.168.1.100:8000/buttonsMeshIDF.bin"
}'
```

### Watch Update

- LED turns blue
- Logs show: "Starting HTTPS OTA update"
- Device restarts with new firmware
- Verify new behavior in logs

---

## Monitor Health (Ongoing)

### Check Peer Health (Root Node)

Watch root logs for:
```
I (xxx) HEALTH_OTA: Peer health: 3/3 peers alive
I (xxx) HEALTH_OTA: Added peer 12345678 to health tracking
W (xxx) HEALTH_OTA: Peer 12345678 timeout (last seen 62000 ms ago)
```

### Check Heap Usage (All Nodes)

Watch logs for:
```
W (xxx) HEALTH_OTA: Low heap detected: 35000 bytes free
E (xxx) HEALTH_OTA: CRITICAL heap level: 18000 bytes free
```

### Test Root Loss Reset (Leaf Node)

1. Power off root node
2. Wait 5+ minutes
3. Leaf should restart automatically
4. Log: "Root lost for 300000 ms, resetting device"

---

## Common Issues

### Gestures Not Detected

**Problem:** No button events on MQTT

**Solutions:**
1. Check mesh connection (green LED)
2. Verify button wiring
3. Check debounce timing in logs
4. Try different button (rule out hardware)

### Configuration Not Persisting

**Problem:** Gestures reset after restart

**Solutions:**
1. Check NVS initialization logs
2. Verify "saved to NVS" message appears
3. Try `nvs_flash_erase()` and reconfigure
4. Check flash partition table

### OTA Fails

**Problem:** OTA starts but fails

**Solutions:**
1. Check URL accessibility from device
2. Verify firmware size < partition size
3. Check certificate (HTTPS only)
4. Monitor heap before OTA
5. Use HTTP for testing (less secure)

### Root Loss Not Triggering

**Problem:** Device doesn't reset after 5 minutes

**Solutions:**
1. Verify this is a leaf node (not root)
2. Check `root_loss_check_task` is running
3. Verify timeout value (5 min default)
4. Check mesh connection status tracking

---

## Advanced Testing

### Stress Test Gestures

```bash
# Rapid button presses
for i in {1..100}; do
  # Press button rapidly
  # Monitor MQTT for all events
  # Verify no dropped presses
done
```

### Multiple Simultaneous OTA

```bash
# Trigger OTA on 5+ nodes at once
# Monitor server load
# Verify all nodes update successfully
```

### Long-Term Stability

```bash
# Run for 24+ hours
# Monitor heap usage trends
# Check peer health accuracy
# Verify no memory leaks
```

---

## Next Steps

1. **Review Documentation:**
   - PHASE5_6_GUIDE.md - Detailed implementation
   - PHASE5_6_TESTING.md - Full test checklist
   - PHASE5_6_SUMMARY.md - Complete overview

2. **Configure Your System:**
   - Set gesture enables per button
   - Configure routing maps
   - Set button types (toggle vs stateful)
   - Test your scenes

3. **Deploy to Production:**
   - Flash all devices
   - Configure via MQTT
   - Monitor health metrics
   - Plan OTA update schedule

---

## Quick Reference

### Gesture Characters

| Gesture | Button 0-6 | Example |
|---------|-----------|---------|
| Single  | a-g       | Button 0 = 'a' |
| Double  | h-n       | Button 0 = 'h' |
| Long    | o-u       | Button 0 = 'o' |

### Gesture Bitmasks

| Bitmask | Enabled Gestures |
|---------|-----------------|
| 1 (0x01) | Single only |
| 3 (0x03) | Single + Double |
| 5 (0x05) | Single + Long |
| 7 (0x07) | All (default) |

### MQTT Topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `/switch/cmd/root` | Publish | Send configs |
| `/switch/state/#` | Subscribe | Button events |
| `/relay/cmd/{id}` | Publish | Relay commands |
| `/relay/state/{id}/{relay}` | Subscribe | Relay states |

### Timing Parameters

| Parameter | Value | Description |
|-----------|-------|-------------|
| Debounce | 250ms | Button press filtering |
| Double window | 400ms | Time between presses |
| Long threshold | 800ms | Hold time for long press |
| Root loss | 5 min | Reset timeout |
| Peer health | 30s | Check interval |
| Heap monitor | 5s | Check interval |

---

## Support

For issues or questions:
1. Check logs with `monitor` command
2. Review PHASE5_6_GUIDE.md troubleshooting
3. Verify configuration JSON syntax
4. Test with minimal setup (1 switch, 1 relay)
5. Check GitHub issues

---

*Quick Start Guide - Phase 5 & 6*  
*ESP-IDF 5.4.0 buttonsMeshIDF Project*  
*Last Updated: February 6, 2026*
