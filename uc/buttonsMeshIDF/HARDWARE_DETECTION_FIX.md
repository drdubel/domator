# Hardware Detection Failure - Relay Board Misdetection

## Issue: ESP32 Relay Board Detected as Switch, Causes WDT Crash

### Symptom
```
I (750) DOMATOR_MESH: Hardware detected as: SWITCH
W (750) DOMATOR_MESH: Cannot distinguish 8-relay from switch
...
I (1026) mesh: 
rst:0x8 (TG1WDT_SYS_RESET)
W (77) boot.esp32: PRO CPU has been reset by WDT
```

Device crashes with watchdog timer reset immediately after mesh initialization starts.

### Root Cause

**Hardware Misdetection Chain:**

1. **Detection fails**: Auto-detection incorrectly identifies 8-relay board as SWITCH
2. **Wrong init sequence**: Switch initialization code runs instead of relay init
3. **GPIO conflicts**: Switch code tries to access button GPIOs that don't exist/work properly
4. **Mesh hangs**: Initialization process hangs or loops indefinitely
5. **Watchdog fires**: Task watchdog timer (15s) expires → system reset

### Technical Details

#### Why Detection Failed (Previous Code)

The old detection logic:
```c
// Checked GPIO 32 but didn't use the result
// Just defaulted to SWITCH
g_node_type = NODE_TYPE_SWITCH;
ESP_LOGW(TAG, "Cannot distinguish 8-relay from switch");
```

#### Key Differences Between Boards

| Feature | Switch Board | 8-Relay Board | 16-Relay Board |
|---------|-------------|---------------|----------------|
| **Primary GPIOs** | 0-6 (buttons) | 32,33,25,26,27,14,12,13 (relays) | 14,13,12,5 (shift register) |
| **Target Chip** | ESP32-C3 | ESP32 | ESP32 |
| **Detection Method** | Default | GPIO 32/33/25 probe | Shift register pins |

---

## Solution Applied

### 1. Improved Auto-Detection

**New detection algorithm:**

```c
1. Check NVS for manual override (hardware_type key)
2. Check for 16-relay: Probe shift register pins (14,13,12)
3. Check for 8-relay: Probe relay-specific GPIOs (32,33,25)
4. On ESP32: Default to RELAY_8 if relay GPIOs accessible
5. On ESP32-C3: Default to SWITCH
```

**Key improvement:** On ESP32 (not ESP32-C3), if GPIOs 32/33/25 are accessible, assume 8-relay board.

### 2. NVS Hardware Type Override

**Purpose:** Allow manual configuration when auto-detection fails.

**How to use:**

```bash
# Connect to device serial console
# In ESP-IDF monitor, press Ctrl+]

# Write hardware type to NVS
nvs_set domator hardware_type u8 1

# Values:
# 0 = SWITCH
# 1 = RELAY_8  (8-relay board)
# 2 = RELAY_16 (16-relay board)
```

Or programmatically:
```c
nvs_handle_t handle;
nvs_open("domator", NVS_READWRITE, &handle);
nvs_set_u8(handle, "hardware_type", 1);  // 1 = RELAY_8
nvs_commit(handle);
nvs_close(handle);
esp_restart();
```

---

## Verification

### Check Detection in Logs

**Successful 8-relay detection:**
```
I (711) DOMATOR_MESH: Starting hardware detection...
I (750) DOMATOR_MESH: Hardware detected as: RELAY_8 (ESP32 with relay GPIOs accessible)
I (755) DOMATOR_MESH: If this is incorrect, set hardware_type in NVS
I (760) DOMATOR_MESH: Pre-initializing relay board (type: 8-relay)
I (770) NODE_RELAY: Initializing relay board
I (780) NODE_RELAY: 8-relay board initialized
I (790) NODE_RELAY: Relay initialization complete - ready for operations
```

**NVS override:**
```
I (711) DOMATOR_MESH: Starting hardware detection...
I (720) DOMATOR_MESH: Hardware type from NVS: RELAY_8 (override)
I (760) DOMATOR_MESH: Pre-initializing relay board (type: 8-relay)
```

**Wrong detection (needs NVS override):**
```
I (750) DOMATOR_MESH: Hardware detected as: SWITCH
I (755) DOMATOR_MESH: Starting switch node tasks
← Wrong! Should configure NVS
```

### Test Relay Functionality

After correct detection:

```bash
# Via MQTT, send relay command
mosquitto_pub -t "/relay/cmd/1074205304" -m "a"

# Should see in logs:
I (xxxx) NODE_RELAY: Received command from root: a
I (xxxx) NODE_RELAY: Toggle relay 0
I (xxxx) NODE_RELAY: Relay 0 set to ON
```

---

## If Auto-Detection Still Fails

### Option 1: Use NVS Override (Recommended)

**Via Python script:**

```python
#!/usr/bin/env python3
import serial
import time

# Connect to ESP32 serial port
ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=1)

# Wait for boot
time.sleep(2)

# Enter NVS set command
# Press Ctrl+] to enter command mode in monitor
# This is automated approach - you can also do manually

print("Connect via idf.py monitor and run:")
print("nvs_set domator hardware_type u8 1")
```

**Manual method:**

1. Connect: `idf.py monitor -p /dev/ttyUSB0`
2. Wait for device to boot and crash/reset
3. Press `Ctrl+]` to enter command mode
4. Type: `nvs_set domator hardware_type u8 1`
5. Press Enter
6. Type: `restart`
7. Device should now boot correctly as RELAY_8

### Option 2: Modify Code to Force Hardware Type

In `domator_mesh.c`, in `detect_hardware_type()`:

```c
void detect_hardware_type(void)
{
    // TEMPORARY: Force hardware type for this specific device
    g_node_type = NODE_TYPE_RELAY;
    g_board_type = BOARD_TYPE_8_RELAY;
    ESP_LOGI(TAG, "Hardware type FORCED to RELAY_8");
    return;
    
    // ... rest of detection code ...
}
```

Then rebuild and flash.

### Option 3: Add Hardware Detection Pin

**Hardware modification:** Add a jumper/switch on a specific GPIO to indicate board type.

Example: Connect GPIO 5 to GND on relay boards, leave floating on switch boards.

```c
// At start of detect_hardware_type()
gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << 5),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
};
gpio_config(&io_conf);

if (gpio_get_level(5) == 0) {
    // GPIO 5 pulled to GND = relay board
    g_node_type = NODE_TYPE_RELAY;
    g_board_type = BOARD_TYPE_8_RELAY;
    return;
}
```

---

## Prevention

### For New Deployments

1. **Test detection immediately**: Check logs on first boot
2. **Set NVS if needed**: Configure hardware_type in NVS during provisioning
3. **Document per device**: Record MAC address and hardware type
4. **Use consistent hardware**: Same board design within deployment

### For Manufacturing

1. **Program NVS during manufacturing**: Include hardware_type in factory NVS partition
2. **Use dedicated detection pin**: Design boards with hardware detection jumper
3. **Test at factory**: Verify correct detection before shipping

---

## Troubleshooting

### Q: Why doesn't auto-detection work 100%?

**A:** GPIO probing can't always distinguish boards:
- Both boards share some GPIO pins
- Some GPIOs have multiple functions
- External connections can affect readings

**Solution:** Use NVS override or hardware detection pin.

### Q: Can I use relay board on ESP32-C3?

**A:** Yes, but:
- Auto-detection skipped on ESP32-C3
- Must configure via NVS: `nvs_set domator hardware_type u8 1`
- Some relay board GPIOs may not be available on ESP32-C3

### Q: What if I set wrong hardware type in NVS?

**A:** Device may malfunction:
- Switch running as relay: Buttons won't work correctly
- Relay running as switch: Relays won't activate

**Fix:** Clear NVS and let auto-detection run, or set correct value:
```bash
nvs_erase domator hardware_type
restart
```

### Q: How to check current NVS value?

**A:** In ESP-IDF monitor:
```bash
nvs_get domator hardware_type u8
```

---

## Related Issues

- **Relay board initialization crash:** See [RELAY_CRASH_FIX.md](RELAY_CRASH_FIX.md)
- **Mesh initialization crash:** See [MESH_INIT_CRASH_FIX.md](MESH_INIT_CRASH_FIX.md)
- **Overall mesh crashes:** See [MESH_CRASH_SUMMARY.md](MESH_CRASH_SUMMARY.md)

---

## Summary

**Problem:** 8-relay boards misdetected as switch → wrong init → watchdog crash

**Solution:** 
1. Improved auto-detection (ESP32 + GPIO 32/33/25 → RELAY_8)
2. NVS override capability (`hardware_type` key)
3. Better logging and error messages

**Quick Fix:** Set hardware type in NVS:
```
nvs_set domator hardware_type u8 1
```

---

*Last Updated: February 7, 2026*  
*Issue: Hardware misdetection causing WDT crash*  
*Status: FIXED with improved detection + NVS override*
