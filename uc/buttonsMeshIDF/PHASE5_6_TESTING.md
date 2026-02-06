# Phase 5 & 6 Testing Checklist

## Pre-Testing Setup

- [ ] Build firmware for ESP32-C3 (switch nodes)
- [ ] Build firmware for ESP32 (relay and root nodes)
- [ ] Configure WiFi credentials in `sdkconfig.defaults`
- [ ] Configure MQTT broker settings
- [ ] Set up MQTT broker (e.g., Mosquitto)
- [ ] Install MQTT client (e.g., mosquitto_pub/sub or MQTT Explorer)

---

## Phase 6 - Gestures Testing

### Gesture Detection

#### Single Press Detection
- [ ] Press button 0 quickly (< 800ms)
- [ ] Verify cyan LED flash
- [ ] Check MQTT `/switch/state/{deviceId}` for character `'a'`
- [ ] Repeat for buttons 1-6 (expect `'b'`-`'g'`)

#### Double Press Detection
- [ ] Press button 0 twice quickly (< 400ms apart)
- [ ] Verify cyan LED flash
- [ ] Check MQTT for character `'h'`
- [ ] Repeat for buttons 1-6 (expect `'i'`-`'n'`)

#### Long Press Detection
- [ ] Hold button 0 for 1+ second
- [ ] Verify cyan LED flash on release
- [ ] Check MQTT for character `'o'`
- [ ] Repeat for buttons 1-6 (expect `'p'`-`'u'`)

### Gesture Configuration

#### Send Configuration via MQTT
- [ ] Subscribe to device logs
- [ ] Send gesture config JSON to `/switch/cmd/root`:
```json
{
  "type": "gesture_config",
  "device_id": "12345678",
  "data": {
    "0": 7,
    "1": 1,
    "2": 3
  }
}
```
- [ ] Check logs for "Gesture configuration saved to NVS"
- [ ] Verify config values in logs

#### Test Disabled Gestures
- [ ] Set button 1 to single-only (bitmask 1)
- [ ] Try double press on button 1
- [ ] Verify single press `'b'` sent instead of `'i'`
- [ ] Try long press on button 1
- [ ] Verify single press `'b'` sent instead of `'p'`

#### Test NVS Persistence
- [ ] Configure gestures via MQTT
- [ ] Restart device (power cycle or esp_restart)
- [ ] Check startup logs for loaded gesture configs
- [ ] Test gestures still work as configured

### Gesture Routing

#### Configure Routing for Gesture Characters
- [ ] Send connections config with gesture characters:
```json
{
  "type": "connections",
  "data": {
    "switchDeviceId": {
      "a": [["relayDeviceId", "a"]],
      "h": [["relayDeviceId", "b"]],
      "o": [["relayDeviceId", "c"]]
    }
  }
}
```
- [ ] Test single press routes to relay `a`
- [ ] Test double press routes to relay `b`
- [ ] Test long press routes to relay `c`
- [ ] Verify all relays respond correctly

---

## Phase 5 - Reliability Testing

### OTA Updates

#### Prepare OTA Firmware
- [ ] Modify firmware version in code
- [ ] Build new firmware binary
- [ ] Host on HTTPS server (nginx, Apache, or cloud storage)
- [ ] Note firmware URL

#### Trigger OTA via MQTT
- [ ] Send OTA trigger to `/switch/cmd/root`:
```json
{
  "type": "ota_trigger",
  "url": "https://example.com/firmware.bin"
}
```
- [ ] Verify device logs show "Starting HTTPS OTA update"
- [ ] Check LED turns blue during OTA
- [ ] Wait for device to restart
- [ ] Verify new firmware version in logs
- [ ] Test device functionality after update

#### Test OTA Error Handling
- [ ] Send OTA trigger with invalid URL
- [ ] Verify error logged and device continues operation
- [ ] Send OTA trigger with wrong firmware (different chip)
- [ ] Verify rollback or error handling

### Peer Health Tracking

#### Basic Health Tracking
- [ ] Start root node
- [ ] Add 2-3 switch/relay nodes to mesh
- [ ] Check root logs for "Added peer X to health tracking"
- [ ] Wait 30+ seconds
- [ ] Verify "Peer health: X/Y peers alive" logged

#### Peer Timeout Detection
- [ ] Power off one leaf node
- [ ] Wait 60+ seconds
- [ ] Check root logs for "Peer X timeout" message
- [ ] Verify disconnect count incremented
- [ ] Power node back on
- [ ] Verify peer marked alive again

#### Peer Health Summary
- [ ] Run mesh with 3+ nodes for 5+ minutes
- [ ] Check periodic health summaries in root logs
- [ ] Verify all nodes reported alive
- [ ] Check peer health data structure

### Root Loss & Reset

#### Normal Operation
- [ ] Start root and leaf nodes
- [ ] Verify green LED on leaf (connected)
- [ ] Monitor leaf logs for root contact updates

#### Root Loss Detection
- [ ] Power off root node
- [ ] Monitor leaf node logs
- [ ] Verify "Root connection lost for X seconds" warnings
- [ ] Wait 5+ minutes
- [ ] Verify leaf node restarts with "Root lost for X ms, resetting device"

#### Root Reconnection
- [ ] Power off root for 2 minutes (< 5 min timeout)
- [ ] Power root back on
- [ ] Verify leaf reconnects without reset
- [ ] Check green LED returns

### Heap Health Monitoring

#### Normal Heap Monitoring
- [ ] Monitor device logs during operation
- [ ] Look for periodic heap status (every 5 seconds)
- [ ] Verify heap stays above thresholds

#### Low Heap Simulation
- [ ] Create artificial heap pressure (large allocations)
- [ ] Watch for "Low heap detected" warnings (< 40KB)
- [ ] Verify warnings throttled (max once per minute)
- [ ] Check statistics counter incremented

#### Critical Heap Simulation
- [ ] Push heap even lower (< 20KB)
- [ ] Watch for "CRITICAL heap level" errors
- [ ] Verify critical event counter incremented
- [ ] Verify device remains stable

---

## Integration Testing

### End-to-End Gesture Scene
- [ ] Configure gesture routing for scene:
  - Button 0 double press → Multiple relays
- [ ] Send configuration
- [ ] Test double press activates all relays
- [ ] Verify all relay states published to MQTT

### Multi-Node Gesture Coordination
- [ ] Configure 2+ switches with gestures
- [ ] Route different gestures to same relay
- [ ] Test all switches control relay correctly
- [ ] Verify no conflicts or race conditions

### OTA During Active Operation
- [ ] Set up mesh with active button presses
- [ ] Trigger OTA on all nodes
- [ ] Verify smooth transition to new firmware
- [ ] Test functionality after update

### Root Loss During Button Press
- [ ] Press button on switch
- [ ] Immediately power off root
- [ ] Verify button press queued
- [ ] Power on root
- [ ] Verify queued press delivered

---

## Performance Testing

### Gesture Response Time
- [ ] Measure time from button press to MQTT publish
- [ ] Verify < 100ms for single press
- [ ] Verify ~400ms delay for single (after double window)
- [ ] Verify long press detected at 800ms

### Configuration Delivery Speed
- [ ] Send gesture config via MQTT
- [ ] Measure time to NVS save completion
- [ ] Verify < 1 second for typical config
- [ ] Test with max size config (all buttons)

### OTA Download Speed
- [ ] Trigger OTA with known firmware size
- [ ] Monitor download progress in logs
- [ ] Calculate download speed
- [ ] Verify reasonable for WiFi connection

### Peer Health Accuracy
- [ ] Disconnect node at known time
- [ ] Verify timeout detected within 70 seconds
  (60s timeout + 10s check interval)
- [ ] Verify no false positives

---

## Stress Testing

### Rapid Gesture Presses
- [ ] Press buttons rapidly (5-10 per second)
- [ ] Verify all presses detected correctly
- [ ] Check for dropped presses
- [ ] Verify no system crashes

### Multiple Simultaneous OTA
- [ ] Trigger OTA on 5+ nodes simultaneously
- [ ] Monitor server load
- [ ] Verify all nodes update successfully
- [ ] Check for timeout issues

### Long-Term Stability
- [ ] Run system for 24+ hours
- [ ] Monitor heap usage over time
- [ ] Check for memory leaks
- [ ] Verify peer health tracking remains accurate
- [ ] Check accumulated statistics

### Configuration Spam
- [ ] Send 10+ configs rapidly
- [ ] Verify device handles gracefully
- [ ] Check final config is correct
- [ ] Verify no NVS corruption

---

## Failure Mode Testing

### Invalid Configurations
- [ ] Send malformed JSON config
- [ ] Verify error logged, device continues
- [ ] Send config with invalid bitmask (> 7)
- [ ] Verify handled gracefully

### Network Interruptions
- [ ] Disconnect WiFi during gesture press
- [ ] Verify queued and sent after reconnect
- [ ] Disconnect during OTA
- [ ] Verify rollback or retry

### Flash Failures
- [ ] Fill NVS to capacity
- [ ] Try to save gesture config
- [ ] Verify error handling
- [ ] Test recovery after NVS erase

### Mesh Topology Changes
- [ ] Add/remove nodes during operation
- [ ] Verify routing adapts
- [ ] Test gestures work with new topology
- [ ] Verify peer health tracking updates

---

## Documentation Verification

- [ ] All MQTT topics documented match implementation
- [ ] Configuration examples are valid JSON
- [ ] Timing values match constants in code
- [ ] Character encoding table is correct
- [ ] Bitmask values documented correctly

---

## Success Criteria

All items above must pass for full Phase 5 & 6 validation.

**Critical Path:**
1. Gesture detection works reliably
2. Gesture config persists in NVS
3. Routing works for all gesture types
4. OTA updates successfully
5. Root loss triggers reset
6. Peer health tracked accurately

---

*Testing Date: _________*
*Tested By: _________*
*Hardware: _________*
*Firmware Version: _________*
*Result: ☐ PASS  ☐ FAIL*

*Notes:*
```


```
