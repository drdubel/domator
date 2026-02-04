# Mesh Network Isolation for Multiple Networks

## Overview

The ESP-NOW mesh network implementation has been enhanced to support running multiple independent networks on the same WiFi infrastructure without interference.

## Problem Statement

When running two separate ESP-NOW mesh networks on the same WiFi SSID:
- Both networks detect the same WiFi channel
- All ESP-NOW devices receive broadcasts from both networks
- Without proper isolation, devices could incorrectly respond to the wrong network
- Potential for cross-network interference and incorrect routing

## Solution Implemented

### Network ID Field

A `networkId` field has been added to the ESP-NOW message structure:

```cpp
typedef struct __attribute__((packed)) {
    uint32_t nodeId;
    uint32_t networkId;  // Network identifier derived from MESH_PASSWORD hash
    uint8_t msgType;
    char data[196];      // Reduced by 4 bytes for networkId
} espnow_message_t;
```

### Network ID Computation

The network ID is computed from a SHA-256 hash of the `MESH_PASSWORD`:

```cpp
uint32_t computeNetworkId(const char* password) {
    unsigned char hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const unsigned char*)password, strlen(password));
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    
    // Use first 4 bytes of hash as network ID
    return (hash[0] << 24) | (hash[1] << 16) | (hash[2] << 8) | hash[3];
}
```

### Message Validation

All received ESP-NOW messages are validated against the expected network ID:

```cpp
void onESPNowDataRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
    espnow_message_t msg;
    memcpy(&msg, data, sizeof(espnow_message_t));

    // Validate network ID to prevent cross-network interference
    if (msg.networkId != networkId) {
        DEBUG_VERBOSE("ESP-NOW: Rejected msg from different network (ID: 0x%08X, expected: 0x%08X)",
                      msg.networkId, networkId);
        return;
    }
    // ... continue processing
}
```

## Configuration for Multiple Networks

To run two separate mesh networks on the same WiFi:

### Network A Configuration

In your `credentials.h` for Network A devices:
```cpp
#define WIFI_SSID "YourWiFiSSID"
#define WIFI_PASSWORD "YourWiFiPassword"
#define MESH_PASSWORD "NetworkA_Password_123"
#define MQTT_USER "network_a_user"
// ... other settings
```

### Network B Configuration

In your `credentials.h` for Network B devices:
```cpp
#define WIFI_SSID "YourWiFiSSID"           // Same WiFi SSID
#define WIFI_PASSWORD "YourWiFiPassword"   // Same WiFi password
#define MESH_PASSWORD "NetworkB_Password_456"  // DIFFERENT mesh password
#define MQTT_USER "network_b_user"
// ... other settings
```

### Key Points

1. **Same WiFi SSID**: Both networks connect to the same WiFi infrastructure
2. **Different MESH_PASSWORD**: Each network MUST have a unique `MESH_PASSWORD`
3. **Network Isolation**: Devices will only communicate with nodes sharing the same `MESH_PASSWORD` hash
4. **No Cross-Talk**: Messages from one network are automatically rejected by the other

## Components Updated

The following components have been updated with network isolation:

1. **rootLightMesh** (`uc/rootLightMesh/src/main.cpp`)
   - Root node that manages the mesh network
   - Broadcasts discovery messages with network ID
   - Validates all incoming messages

2. **relayLightMesh** (`uc/relayLightMesh/src/main.cpp`)
   - Relay nodes with 8 or 16 output channels
   - Validates network ID on all received messages
   - Only registers with root nodes from same network

3. **buttonLightMesh** (`uc/buttonLightMesh/src/main.cpp`)
   - Switch/button nodes (ESP32-C3)
   - Validates network ID on all received messages
   - Only registers with root nodes from same network

## Technical Details

### Message Structure Changes

- **Before**: 205 bytes (4B nodeId + 1B msgType + 200B data)
- **After**: 205 bytes (4B nodeId + 4B networkId + 1B msgType + 196B data)
- Total size remains the same, maintaining compatibility with ESP-NOW limits

### Security Considerations

1. **Network ID is not encryption**: The network ID provides isolation but not security
2. **MESH_PASSWORD is visible**: The password is used for network identification, not encryption
3. **ESP-NOW is unencrypted**: All messages are sent in plain text (encryption is disabled)
4. **For production**: Consider enabling ESP-NOW encryption with unique PMK per network

### Performance Impact

- **Minimal overhead**: Network ID validation adds ~10 CPU cycles per message
- **No additional network traffic**: Network ID is included in existing message structure
- **Same memory footprint**: Message structure size unchanged

## Testing Checklist

- [ ] Compile all three components successfully
- [ ] Test single network functionality
- [ ] Configure two networks with different MESH_PASSWORD values
- [ ] Verify Network A root does not accept Network B nodes
- [ ] Verify Network B root does not accept Network A nodes
- [ ] Test simultaneous operation of both networks
- [ ] Monitor debug logs for network ID rejection messages

## Debug Output

When network isolation is working correctly, you should see:

```
[INFO] Network ID: 0x12345678 (derived from MESH_PASSWORD)
[VERBOSE] ESP-NOW: Rejected msg from different network (ID: 0x87654321, expected: 0x12345678)
```

## Future Enhancements

Potential improvements for enhanced isolation:

1. **ESP-NOW Encryption**: Enable encryption with network-specific PMK
2. **HMAC Signatures**: Add cryptographic signatures to handshake messages
3. **Network ID in Discovery**: Include network name in discovery broadcasts
4. **Collision Detection**: Detect and warn about nodeId collisions across networks
5. **Channel Separation**: Allow different networks to use different WiFi channels when possible

## Troubleshooting

### Devices Not Connecting

**Symptom**: Nodes not registering with root
**Solution**: Verify all devices in the network have identical `MESH_PASSWORD`

### Cross-Network Communication

**Symptom**: Devices responding to wrong network
**Solution**: Ensure each network has unique `MESH_PASSWORD`, recompile and reflash

### Network ID Collision

**Symptom**: Two networks with different passwords have same network ID (very rare)
**Solution**: Change one of the `MESH_PASSWORD` values to generate different hash

## References

- ESP-NOW Protocol: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html
- mbedTLS SHA-256: https://tls.mbed.org/api/sha256_8h.html
