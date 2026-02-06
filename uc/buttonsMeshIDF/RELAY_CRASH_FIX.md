# Relay Board Reset/Crash Troubleshooting Guide

## Issue: Relay Board Crashes with "I (1263) mesh:" Message

### Symptom
- Relay board resets during startup
- Last message before reset: `I (1263) mesh:`
- Appears to crash during mesh initialization

### Root Cause
The issue was caused by a **race condition** during initialization:

1. Mesh network starts before relay hardware is initialized
2. Mesh events trigger relay operations (e.g., commands from MQTT)
3. Relay functions try to use uninitialized mutex → **CRASH**
4. Insufficient stack size for mesh tasks under load

### Fix Applied (Version 1.1.0+)

#### 1. Fixed Initialization Order
**Before (WRONG):**
```
1. Create relay mutex
2. Start mesh network ← Mesh events can trigger relay ops
3. Initialize relay hardware ← Too late!
4. Start relay tasks
```

**After (CORRECT):**
```
1. Create relay mutex
2. Initialize relay hardware ← Ready before mesh
3. Start mesh network ← Safe now
4. Start relay tasks
```

#### 2. Added Safety Checks
All relay functions now check:
- `g_relay_initialized` flag (prevents operations before init)
- `g_relay_mutex` exists (prevents null pointer crash)

#### 3. Increased Stack Sizes
- `mesh_send_task`: 4096 → **5120 bytes**
- `mesh_recv_task`: 4096 → **5120 bytes**
- `relay_button_task`: 4096 → **5120 bytes**

---

## Verification Steps

After applying the fix, verify:

### 1. Check Initialization Order in Logs
```
I (xxx) DOMATOR_MESH: Device ID: 12345678
I (xxx) DOMATOR_MESH: Hardware detected as: RELAY_8
I (xxx) DOMATOR_MESH: Pre-initializing relay board (type: 8-relay)
I (xxx) NODE_RELAY: Initializing relay board
I (xxx) NODE_RELAY: 8-relay board initialized
I (xxx) NODE_RELAY: Relay initialization complete - ready for operations
I (xxx) MESH_INIT: Initializing mesh network
I (xxx) MESH_INIT: Mesh started
```

### 2. No Crashes During Mesh Events
Monitor for these without crashes:
```
I (xxx) MESH_INIT: Parent connected - Layer: 2
I (xxx) NODE_RELAY: Received command from root: a
I (xxx) NODE_RELAY: Toggle relay 0
```

### 3. Check Memory Usage
```
I (xxx) HEALTH_OTA: Free heap: 150000 bytes
```
Should remain stable > 100KB.

---

## If Problem Persists

### Check 1: Stack Overflow
Look for `***ERROR*** A stack overflow in task` in logs.

**Solution:** Increase stack further in `domator_mesh.c`:
```c
xTaskCreate(mesh_send_task, "mesh_send", 6144, NULL, 5, NULL);
xTaskCreate(mesh_recv_task, "mesh_recv", 6144, NULL, 5, NULL);
```

### Check 2: GPIO Configuration Failure
Look for `Failed to configure GPIO` messages.

**Solution:** 
- Verify correct board type detected
- Check for GPIO conflicts
- Ensure ESP32 (not ESP32-C3) for relay boards

### Check 3: Insufficient Heap
Look for `ESP_ERR_NO_MEM` errors.

**Solution:**
- Reduce `MESH_TX_QUEUE_SIZE` from 20 to 10
- Reduce `MAX_QUEUE_SIZE` from 30 to 20
- Disable debug logging: Set log level to INFO

### Check 4: Watchdog Timeout
Look for `Task watchdog got triggered` messages.

**Solution:**
- Reduce operations in relay initialization
- Add `vTaskDelay()` in initialization loops
- Disable watchdog during init (not recommended)

---

## Quick Diagnostic Commands

### Check Current Firmware Version
```bash
# In device logs, look for:
I (xxx) DOMATOR_MESH: Firmware version: vX.X.X, hash: abc123...
```

If version is before this fix (< 1.1.0), update firmware.

### Force Clean Build
```bash
cd buttonsMeshIDF
idf.py fullclean
idf.py build
idf.py flash monitor
```

### Check Partition Table
```bash
idf.py partition-table
```

Should show:
```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x5000
otadata,  data, ota,     0xe000,  0x2000
ota_0,    app,  ota_0,   0x10000, 0x160000
ota_1,    app,  ota_1,   0x170000,0x160000
```

---

## Advanced Debugging

### Enable Core Dump
In `sdkconfig`:
```
CONFIG_ESP_COREDUMP_ENABLE=y
CONFIG_ESP_COREDUMP_TO_FLASH_OR_UART=y
```

### Enable Stack Canary
```
CONFIG_COMPILER_STACK_CHECK_MODE_STRONG=y
```

### Increase Watchdog Timeout
```
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10
```

### Monitor Stack High Water Mark
Add to code:
```c
UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
ESP_LOGI(TAG, "Stack high water mark: %lu", hwm);
```

---

## Prevention

### For Future Development

1. **Always initialize hardware BEFORE starting network services**
2. **Use initialization flags** to prevent premature operations
3. **Check for null pointers** before using mutexes
4. **Test with various timing** (fast/slow WiFi connection)
5. **Monitor stack usage** during development

### Code Review Checklist

When adding new features:
- [ ] Hardware init before mesh_init()?
- [ ] Mutex created before use?
- [ ] Initialization flag checked?
- [ ] Stack size adequate? (minimum 4KB, prefer 5KB+)
- [ ] Error handling for all ESP_ERROR_CHECK()?

---

## Related Files Modified

- `src/domator_mesh.c` - Initialization order fix
- `src/node_relay.c` - Safety checks added
- This document - `RELAY_CRASH_FIX.md`

---

## Version History

- **v1.0.x**: Initial implementation (had race condition)
- **v1.1.0**: Fixed initialization order and added safety checks
- **Current**: Stable relay operation

---

*Last Updated: February 6, 2026*
*Issue: Relay board crash during mesh initialization*
*Status: FIXED in v1.1.0+*
