#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define MESH_MSG_DATA_SIZE 512

// Message types for mesh communication
#define MSG_TYPE_BUTTON 'B'   // Button press from switch to root
#define MSG_TYPE_STATUS 'S'   // Status update from nodes to root
#define MSG_TYPE_COMMAND 'C'  // Command from root to relay (e.g., toggle relay)
#define MSG_TYPE_ACK 'A'      // Acknowledgment for command receipt
#define MSG_TYPE_RELAY_STATE 'R'   // Relay state confirmation
#define MSG_TYPE_SYNC_REQUEST 'Y'  // Request state sync
#define MSG_TYPE_CONFIG 'G'        // Configuration message
#define MSG_TYPE_TYPE_INFO 'T'     // Message to convey device type info
#define MSG_TYPE_OTA_START 'U'     // OTA update start packet
#define MSG_TYPE_PING 'P'          // Ping message for health check'
#define MSG_TYPE_ESPNOW 'E'        // Authenticated remote packet / return command

// Device types for type info messages
#define DEVICE_TYPE_SWITCH 'S'
#define DEVICE_TYPE_RELAY 'R'

/** @brief Wire-format application message exchanged between mesh nodes. */
typedef struct {
    uint64_t src_id;
    uint8_t msg_type;
    uint16_t data_len;
    uint32_t data_seq;
    uint8_t target_type;
    char data[MESH_MSG_DATA_SIZE];
} __attribute__((packed)) mesh_app_msg_t;

/* Validate before logging, registry updates, or dispatch. No IDF dependency. */
static inline int mesh_message_valid(const mesh_app_msg_t* msg, size_t size) {
    if (size != sizeof(*msg) || !msg->src_id ||
        msg->data_len > sizeof(msg->data) ||
        (msg->target_type != 0 && msg->target_type != DEVICE_TYPE_RELAY &&
         msg->target_type != DEVICE_TYPE_SWITCH)) return 0;
    switch (msg->msg_type) {
        case MSG_TYPE_BUTTON:
            return msg->data_len >= 1 && msg->data_len <= 3 &&
                msg->data[0] >= 'a' && msg->data[0] <= 'x' &&
                (msg->data_len < 2 || msg->data[1] == '0' || msg->data[1] == '1') &&
                (msg->data_len < 3 || msg->data[2] == '0' || msg->data[2] == '1');
        case MSG_TYPE_RELAY_STATE:
            return msg->data_len == 2 && msg->data[0] >= 'A' && msg->data[0] <= 'P' &&
                (msg->data[1] == '0' || msg->data[1] == '1');
        case MSG_TYPE_PING: return msg->data_len == sizeof(uint16_t);
        case MSG_TYPE_TYPE_INFO:
            return msg->data_len == 1 &&
                (msg->data[0] == DEVICE_TYPE_SWITCH || msg->data[0] == DEVICE_TYPE_RELAY);
        case MSG_TYPE_SYNC_REQUEST:
        case MSG_TYPE_OTA_START: return msg->data_len == 0;
        case MSG_TYPE_COMMAND:
        case MSG_TYPE_STATUS:
            return msg->data_len > 0 && msg->data_len < sizeof(msg->data) &&
                memchr(msg->data, 0, msg->data_len) == NULL;
        case MSG_TYPE_ESPNOW: return msg->data_len == 250;
        default: return 0;
    }
}

static inline void mesh_terminate_text(mesh_app_msg_t* msg) {
    if (msg->msg_type == MSG_TYPE_COMMAND || msg->msg_type == MSG_TYPE_STATUS)
        msg->data[msg->data_len] = 0;
}
