# ESP-WIFI-MESH Crash Issues - Complete Summary

This document summarizes the two related but distinct mesh initialization crash issues and their fixes.

---

## Issue 1: Relay Board Initialization Race Condition

### Symptom
```
I (1263) mesh: 
[Device resets]
```

Occurs during relay board startup, typically within first 2 seconds.

### Root Cause
**Race condition:** Mesh network starts before relay hardware initializes. When mesh events trigger relay operations, uninitialized mutex causes crash.

### Solution
- Move `relay_init()` **before** `mesh_init()` in initialization sequence
- Add `g_relay_initialized` flag and safety checks
- Increase task stack sizes to 5120 bytes

### Documentation
See [RELAY_CRASH_FIX.md](RELAY_CRASH_FIX.md)

---

## Issue 2: Mesh Initialization Memory Exhaustion

### Symptom
```
I (968) MESH_INIT: WiFi STA started
I (971) mesh: 
[Device resets]
```

Occurs immediately after WiFi STA starts, before mesh fully initializes.

### Root Cause
**Memory exhaustion:** WiFi buffers (32 RX + 32 TX = ~102KB) plus mesh structures (~40KB) exceed available heap (~120KB after WiFi init).

### Solution
- Reduce WiFi dynamic buffers from 32 to 16 (both RX and TX)
- Disable AMSDU TX aggregation
- Add heap monitoring and diagnostic logging
- Increase watchdog timeouts

### Documentation
See [MESH_INIT_CRASH_FIX.md](MESH_INIT_CRASH_FIX.md)

---

## How to Distinguish Issues

### Check the Timing

**Issue 1 (Relay Race Condition):**
- Occurs at ~1200ms
- Only on relay boards
- After "mesh:" log from ESP-IDF

**Issue 2 (Memory Exhaustion):**
- Occurs at ~970ms
- Any node type
- After "WiFi STA started"

### Check Node Type

**Issue 1:** Only affects relay boards (NODE_TYPE_RELAY)  
**Issue 2:** Affects all node types (root, switch, relay)

### Check Configuration

**Issue 1:** Related to hardware initialization order  
**Issue 2:** Related to WiFi buffer memory allocation

---

## Combined Fix Verification

After applying both fixes, your logs should show:

### Successful Startup Sequence

```
I (100) DOMATOR_MESH: Domator Mesh starting...
I (150) DOMATOR_MESH: Device ID: 12345678
I (160) DOMATOR_MESH: Hardware detected as: RELAY_8

# Issue 1 Fix - Relay before mesh
I (170) DOMATOR_MESH: Pre-initializing relay board (type: 8-relay)
I (180) NODE_RELAY: Initializing relay board
I (200) NODE_RELAY: Relay initialization complete - ready for operations

# Issue 2 Fix - Heap monitoring
I (250) MESH_INIT: Initializing mesh network
I (260) MESH_INIT: Free heap before mesh init: 180000 bytes
I (500) MESH_INIT: WiFi STA started
I (510) MESH_INIT: Free heap after WiFi init: 130000 bytes
I (520) MESH_INIT: Calling esp_mesh_init()...
I (800) MESH_INIT: esp_mesh_init() completed
I (810) MESH_INIT: Configuring mesh parameters...
I (820) MESH_INIT: Starting mesh network...
I (900) MESH_INIT: Mesh initialized
I (910) MESH_INIT: Free heap after mesh init: 110000 bytes

# Mesh operation
I (2000) MESH_INIT: Mesh started
I (5000) MESH_INIT: Parent connected - Layer: 2
```

**Key indicators:**
- ✅ Relay init **before** mesh init
- ✅ Heap > 120KB after WiFi init
- ✅ Heap > 100KB after mesh init
- ✅ No crashes or resets
- ✅ Mesh successfully connects

---

## Quick Diagnostic Checklist

### If Device Still Crashes

1. **Check heap after WiFi init**
   - Required: > 120KB
   - If less: Reduce buffers further or reduce application memory

2. **Check initialization order in logs**
   - For relay boards: "Pre-initializing relay board" must appear before "Initializing mesh network"
   - If wrong order: Update firmware to include relay init fix

3. **Check for stack overflow**
   - Look for: `***ERROR*** A stack overflow in task`
   - If present: Increase stack sizes further (6144 or 8192)

4. **Check total heap at boot**
   - Required: > 150KB initially
   - If less: Reduce static allocations or use SPIRAM

5. **Verify configuration**
   - Use provided `sdkconfig.defaults`
   - Do clean rebuild: `idf.py fullclean build`

---

## Configuration Reference

### Minimal Working Configuration

```ini
# WiFi - Optimized for memory
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=16
CONFIG_ESP_WIFI_AMSDU_TX_ENABLED=n

# Mesh - Standard
CONFIG_ESP_WIFI_MESH_ENABLE=y
CONFIG_MESH_IE_CRYPTO_FUNCS=y

# Watchdogs - Increased
CONFIG_ESP_TASK_WDT_TIMEOUT_S=15
CONFIG_ESP_INT_WDT_TIMEOUT_MS=800
```

### Task Stack Sizes

```c
// In domator_mesh.c
xTaskCreate(mesh_send_task, "mesh_send", 5120, NULL, 5, NULL);
xTaskCreate(mesh_recv_task, "mesh_recv", 5120, NULL, 5, NULL);
xTaskCreate(relay_button_task, "relay_button", 5120, NULL, 6, NULL);
```

---

## Prevention Guidelines

### For Future Development

1. **Always initialize hardware before network services**
   - Hardware → Mutexes → Network → Tasks

2. **Monitor heap at critical points**
   - After each major initialization step
   - Warn if < 80KB available

3. **Use adequate stack sizes**
   - Minimum 4KB for simple tasks
   - 5KB+ for network/mesh tasks
   - 6-8KB for tasks with recursion

4. **Test with memory constraints**
   - Simulate low heap conditions
   - Test with various WiFi buffer configs
   - Monitor minimum heap over time

5. **Document resource requirements**
   - Heap requirements per feature
   - Stack requirements per task
   - Buffer configuration trade-offs

---

## Related Resources

- [RELAY_CRASH_FIX.md](RELAY_CRASH_FIX.md) - Relay initialization race condition
- [MESH_INIT_CRASH_FIX.md](MESH_INIT_CRASH_FIX.md) - Mesh memory exhaustion
- [README.md](README.md) - Project overview and features
- [QUICKSTART.md](QUICKSTART.md) - Quick setup guide
- [PHASE5_6_GUIDE.md](PHASE5_6_GUIDE.md) - Advanced features

---

## Version History

- **v1.0.x**: Initial release (both issues present)
- **v1.1.0**: Fixed relay initialization race condition
- **v1.1.1**: Fixed mesh initialization memory exhaustion
- **Current**: Both issues resolved, stable operation

---

## Summary

Both crash issues are now resolved:

1. **Relay race condition** → Fixed by initialization order
2. **Mesh memory exhaustion** → Fixed by reduced WiFi buffers

Update to latest firmware and use provided configuration files for stable operation.

---

*Last Updated: February 6, 2026*  
*Status: Both issues FIXED*  
*Recommended: Use v1.1.1+ firmware*
