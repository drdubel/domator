# Mesh Network Troubleshooting Guide

This guide helps diagnose and fix common issues with the ESP-WIFI-MESH network.

---

## Quick Diagnostic Checklist

Run through this checklist to quickly identify your issue:

- [ ] **WiFi configured?** Check `sdkconfig` for `CONFIG_WIFI_SSID` and `CONFIG_WIFI_PASSWORD`
- [ ] **IP address correct?** Should be 192.168.1.x or 192.168.0.x, NOT 192.168.4.x
- [ ] **MQTT broker running?** Test with `telnet BROKER_IP 1883`
- [ ] **Mesh formed?** Check for "Mesh is connected" in logs
- [ ] **Root elected?** Look for "Root got IP" message
- [ ] **Devices communicating?** Try sending button press or relay command

---

## Common Issues

### 1. IP Address is 192.168.4.x (Isolated Mode)

**Symptom:**
```
I (7245) esp_netif_handlers: sta ip: 192.168.4.2, mask: 255.255.255.0, gw: 192.168.4.1
W (26245) NODE_ROOT: MQTT error
E (76880) esp-tls: [sock=48] select() timeout
```

**Meaning:**
- Mesh operating in isolated mode
- NOT connected to home WiFi router
- MQTT cannot reach broker

**Root Cause:**
- WiFi credentials not configured or incorrect
- Router out of range
- Router 2.4GHz band disabled

**Solution:**
See **IP_ADDRESSING_GUIDE.md** for detailed fix. Quick solution:
```bash
idf.py menuconfig
# → Domator Mesh Configuration → WiFi SSID/Password
idf.py build flash monitor
```

**Verification:**
After fix, should see:
```
sta ip: 192.168.1.45, gw: 192.168.1.1  ← Home network
MQTT connected                          ← Success
```

---

### 2. MQTT Connection Failures

**Symptom:**
```
E (56871) NODE_ROOT: MQTT error
E (76880) esp-tls: [sock=48] select() timeout
E (76881) transport_base: Failed to open a new connection: 32774
E (76881) mqtt_client: Error transport connect
```

**Root Causes:**

#### A. No MQTT Broker Running
**Check:**
```bash
# Test broker connectivity
telnet 192.168.1.100 1883

# Check if Mosquitto is running
sudo systemctl status mosquitto
```

**Solution:**
```bash
# Install and start Mosquitto
sudo apt-get install mosquitto
sudo systemctl start mosquitto
```

See **MQTT_TROUBLESHOOTING.md** for detailed setup.

#### B. Wrong Broker IP Address
**Check:**
```bash
# View current configuration
idf.py menuconfig
# → Domator Mesh Configuration → MQTT Broker URL
```

**Solution:**
Update to correct IP and rebuild:
```bash
idf.py build flash monitor
```

#### C. Firewall Blocking Port 1883
**Check:**
```bash
# Check firewall rules
sudo ufw status
sudo iptables -L -n | grep 1883
```

**Solution:**
```bash
# Allow MQTT port
sudo ufw allow 1883
```

#### D. Device Not on Same Network
**Check:**
- ESP32 IP: 192.168.1.45
- Broker IP: 192.168.1.100
- Must be same subnet!

**Solution:**
Fix WiFi configuration (see Issue #1 above)

---

### 3. Mesh Not Forming

**Symptom:**
```
W (30000) mesh: <MESH_EVENT_PARENT_DISCONNECTED>reason:205
W (35000) mesh: <MESH_EVENT_PARENT_DISCONNECTED>reason:205
```

**Root Causes:**

#### A. Incompatible Mesh IDs
All devices must have same `MESH_ID` configured.

**Check:**
```c
// In sdkconfig or menuconfig
CONFIG_MESH_ID="77:77:77:77:77:77"
```

**Solution:**
Ensure all devices have identical MESH_ID, rebuild all:
```bash
idf.py build flash monitor
```

#### B. Different Mesh Channels
**Check:**
```bash
idf.py menuconfig
# → Domator Mesh Configuration → Mesh Channel
```

**Solution:**
Use same channel on all devices (default: 1)

#### C. Signal Too Weak
**Symptoms:**
- Devices keep disconnecting
- Frequent parent changes

**Solution:**
- Move devices closer together
- Improve antenna orientation
- Reduce physical obstacles

---

### 4. Root Election Issues

**Symptom:**
```
I (10000) MESH_INIT: Waiting to become root or get parent...
I (20000) MESH_INIT: Waiting to become root or get parent...
```

**Root Causes:**

#### A. No Device Can Reach Router
**Check:**
- Is router powered on?
- Is 2.4GHz WiFi enabled?
- Are credentials correct?

**Solution:**
1. Verify router WiFi settings
2. Check WiFi credentials configuration
3. Move at least one device closer to router

#### B. All Devices Have Weak Signal
**Check:**
```
W (10000) wifi: no AP found
```

**Solution:**
- Position at least one device within good range of router
- That device will become root

#### C. Root Keeps Switching
**This is normal** if multiple devices have similar signal strength.

**To minimize:**
- Give one device better positioning (strongest signal)
- That device will tend to stay root

---

### 5. Device Communication Failures

**Symptom:**
- Button press doesn't control relay
- MQTT command doesn't reach device

**Root Causes:**

#### A. Device Not in Peer Health Table
**Check logs for:**
```
I (30000) NODE_ROOT: Peer health update: device 1074207536
```

**Solution:**
- Device must send at least one message to be tracked
- Press button or restart device to register

#### B. Device ID Mismatch
**Check:**
- MQTT command targets: `/relay/cmd/1074207536`
- Actual device ID from logs: `Device ID: 1074205304`

**Solution:**
Use correct device ID from serial logs

#### C. Mesh Network Disconnected
**Check:**
```
W (45000) mesh: <MESH_EVENT_ROOT_LOST>
```

**Solution:**
- Root node lost connection
- Wait for new root election (10 seconds)
- Check root node power and WiFi

---

### 6. Hardware Misdetection

**Symptom:**
```
I (750) DOMATOR_MESH: Hardware detected as: SWITCH
W (750) DOMATOR_MESH: Cannot distinguish 8-relay from switch
```
Then device crashes or behaves wrong.

**Root Cause:**
Relay board incorrectly detected as switch.

**Solution:**
See **HARDWARE_DETECTION_FIX.md** for detailed fix. Quick solution:

```bash
# Option 1: NVS override
idf.py monitor
# In monitor, run: nvs_set domator hardware_type u8 1
# Then: restart

# Option 2: Force in code
# Edit domator_mesh.c, in detect_hardware_type():
g_node_type = NODE_TYPE_RELAY;
g_board_type = BOARD_TYPE_8_RELAY;
```

---

### 7. Crashes and Resets

#### A. Watchdog Timer Reset

**Symptom:**
```
rst:0x8 (TG1WDT_SYS_RESET)
W (77) boot.esp32: PRO CPU has been reset by WDT
```

**See:** MESH_CRASH_SUMMARY.md for detailed analysis.

**Common causes:**
1. Hardware misdetection (relay detected as switch)
2. Memory exhaustion (WiFi buffers too large)
3. Race condition (relay init after mesh)

**Solutions:**
- Fix hardware detection (HARDWARE_DETECTION_FIX.md)
- Reduce WiFi buffers (MESH_INIT_CRASH_FIX.md)
- Correct initialization order (RELAY_CRASH_FIX.md)

#### B. Stack Overflow

**Symptom:**
```
***ERROR*** A stack overflow in task XXX has been detected.
```

**Solution:**
Already fixed - task stack sizes increased to 5120 bytes.

If still occurring:
```c
// In domator_mesh.c, increase further:
xTaskCreate(mesh_send_task, "mesh_send", 6144, NULL, 5, NULL);
xTaskCreate(mesh_recv_task, "mesh_recv", 6144, NULL, 5, NULL);
```

#### C. Heap Exhaustion

**Symptom:**
```
E (1026) mesh: esp_mesh_init() failed
```

**Solution:**
WiFi buffers reduced from 32 to 16 - should be fixed.

**Verify:**
Check logs for heap availability:
```
I (760) MESH_INIT: Free heap before mesh init: 258712 bytes
I (998) MESH_INIT: Free heap after WiFi init: 210676 bytes
```

Should have > 100KB after WiFi init.

---

## Advanced Diagnostics

### Enable Verbose Logging

In `idf.py menuconfig`:
```
Component config → Log output
  → Default log verbosity → Debug
```

Then rebuild and monitor:
```bash
idf.py build flash monitor
```

### Monitor Heap Usage

Add to your code:
```c
ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
ESP_LOGI(TAG, "Min free heap: %lu bytes", esp_get_minimum_free_heap_size());
```

### Check Mesh Events

Monitor for these important events:
```
<MESH_EVENT_STARTED>             - Mesh initialized
<MESH_EVENT_PARENT_CONNECTED>    - Connected to parent
<MESH_EVENT_ROOT_GOT_IP>        - Root got IP from router
<MESH_EVENT_ROOT_LOST>          - Root lost connection
<MESH_EVENT_PARENT_DISCONNECTED> - Lost connection to parent
```

### Network Packet Capture

Use Wireshark:
1. Capture on WiFi interface
2. Filter: `wlan.mesh` or `ip.addr == ESP32_IP`
3. Analyze packet flow and timing

### MQTT Message Tracing

Subscribe to all topics:
```bash
mosquitto_sub -h BROKER_IP -t "#" -v
```

Watch for:
- Button messages from switches
- Relay status from relay boards
- Connection status from root
- Command messages

---

## Performance Optimization

### Reduce Network Traffic

**Device-Specific Targeting:**
Use device ID in MQTT topic to send commands directly:
```bash
# Targeted (efficient)
mosquitto_pub -t "/relay/cmd/1074207536" -m "a1"

# Broadcast (inefficient)
mosquitto_pub -t "/relay/cmd" -m "a1"
```

### Mesh Network Sizing

**Recommended limits:**
- Max devices: 50
- Max children per node: 6
- Max mesh layers: 6

**Exceeding limits may cause:**
- Slower response times
- Increased packet loss
- Routing failures

### WiFi Channel Selection

**Best practices:**
- Use channel 1, 6, or 11 (non-overlapping)
- Avoid crowded channels
- Check with WiFi analyzer app

### Power Management

For battery-powered devices:
```c
// Enable light sleep
esp_pm_config_esp32_t pm_config = {
    .max_freq_mhz = 80,
    .min_freq_mhz = 10,
    .light_sleep_enable = true
};
esp_pm_configure(&pm_config);
```

---

## Monitoring Best Practices

### 1. Connection Status Monitoring

Subscribe to status topic:
```bash
mosquitto_sub -t "/status/root/connection" -v
```

Expected:
```json
{"status":"connected","device_id":1074205304,"timestamp":1707308070,"firmware":"77626c3","ip":"192.168.1.45","mesh_layer":1}
```

### 2. Device Health Tracking

Root node tracks all devices automatically. Check logs:
```
I (30000) NODE_ROOT: Peer health update: device 1074207536, last_seen=...
I (30000) NODE_ROOT: Active peers: 3
```

### 3. Periodic Testing

Automate testing with script:
```bash
#!/bin/bash
# Test relay control
mosquitto_pub -t "/relay/cmd/1074207536" -m "a1"
sleep 1
mosquitto_pub -t "/relay/cmd/1074207536" -m "a0"

# Check if device responds
# (monitor MQTT messages or check physical relay)
```

### 4. Log Aggregation

Collect logs from all devices:
```bash
# ESP-IDF
idf.py monitor > device1.log 2>&1 &

# Then analyze
grep ERROR device1.log
grep WARNING device1.log
```

---

## Getting Help

### Information to Provide

When reporting issues, include:

1. **Full logs** from startup to error
2. **Hardware type** (ESP32, ESP32-C3, relay, switch)
3. **Network configuration** (IP addresses, gateway, MQTT broker)
4. **ESP-IDF version** (from logs: `IDF Version: 5.4.0`)
5. **Firmware version** (from logs: `Firmware version: ...`)
6. **Configuration** (relevant parts of `sdkconfig`)

### Useful Log Sections

```bash
# Save complete logs
idf.py monitor > full_log.txt 2>&1

# Extract key sections
grep "MESH" full_log.txt > mesh_events.txt
grep "MQTT" full_log.txt > mqtt_events.txt
grep "ERROR\|WARNING" full_log.txt > errors.txt
```

### Related Documentation

- **IP_ADDRESSING_GUIDE.md** - Understanding network configuration
- **MQTT_TROUBLESHOOTING.md** - MQTT-specific issues
- **ROOT_ELECTION.md** - Root node election details
- **HARDWARE_DETECTION_FIX.md** - Hardware misdetection issues
- **MESH_CRASH_SUMMARY.md** - Crash and reset issues
- **DEVICE_TARGETING.md** - Device-specific command routing
- **README.md** - Project overview and setup

---

## Summary

**Most Common Issues:**
1. ❌ WiFi not configured → IP is 192.168.4.x → Fix: Configure WiFi
2. ❌ MQTT broker not running → Connection timeout → Fix: Install/start Mosquitto
3. ❌ Wrong broker IP → Connection fails → Fix: Update configuration
4. ❌ Hardware misdetection → Crashes → Fix: NVS override or code fix

**Quick Health Check:**
```bash
# 1. Check IP (should be your network range, not 192.168.4.x)
# 2. Check MQTT (should see "MQTT connected")
# 3. Test command (should control device)
mosquitto_pub -t "/relay/cmd" -m "a1"
```

**If still having issues:**
1. Check relevant guide above
2. Enable debug logging
3. Collect full logs
4. Review error messages carefully

Most issues are configuration-related and easily fixed!
