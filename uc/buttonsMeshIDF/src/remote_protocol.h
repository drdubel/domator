#pragma once
#include <stddef.h>
#include <stdint.h>

#define REMOTE_MAGIC 0x44524d31u
#define REMOTE_VERSION 1
#define REMOTE_PAYLOAD_SIZE 210
#define REMOTE_HEARTBEAT_US (60ULL * 1000000ULL)
#define REMOTE_ACK_WAIT_MS 45
#define REMOTE_ATTEMPTS 3
#define REMOTE_DEVICE_TYPE 'N'

enum { REMOTE_UPLINK = 1, REMOTE_ACK, REMOTE_COMMAND, REMOTE_COMMAND_ACK };
/* Fixed-size, zero-filled packets fit ESP-NOW v1's 250-byte limit.
 * payload[0] is the mesh message type; the remainder is its payload.
 * HMAC-SHA256/128 authenticates all bytes preceding tag. */
typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t kind;
    uint16_t length;
    uint64_t device_id;
    uint32_t session;
    uint32_t sequence;
    uint8_t payload[REMOTE_PAYLOAD_SIZE];
    uint8_t tag[16];
} __attribute__((packed)) remote_packet_t;

_Static_assert(sizeof(remote_packet_t) <= 250, "ESP-NOW v1 packet too large");

static inline int remote_packet_shape_valid(const remote_packet_t* p) {
    return p->magic == REMOTE_MAGIC && p->version == REMOTE_VERSION &&
           p->device_id && p->device_id <= UINT64_C(0xffffffffffff) &&
           p->session && p->sequence && p->length <= REMOTE_PAYLOAD_SIZE &&
           p->kind >= REMOTE_UPLINK && p->kind <= REMOTE_COMMAND_ACK;
}

/* Keep validation independent of IDF so malformed frames and retry behavior
 * can be checked on the host without flashing or radio hardware. */
static inline int remote_uplink_valid(const remote_packet_t* p) {
    if (!remote_packet_shape_valid(p) || p->kind != REMOTE_UPLINK || !p->length) return 0;
    switch (p->payload[0]) {
        case 'B':
            return (p->length == 3 || p->length == 4) &&
                p->payload[1] >= 'a' && p->payload[1] <= 'g' &&
                (p->payload[2] == '0' || p->payload[2] == '1') &&
                (p->length == 3 || p->payload[3] == '0' || p->payload[3] == '1');
        case 'P': return p->length == 3;
        case 'S': return p->length > 1;
        default: return 0;
    }
}

typedef struct { uint64_t id; uint32_t session, sequence; } remote_history_t;
static inline int remote_duplicate(remote_history_t* h, const remote_packet_t* p) {
    if (h->id == p->device_id && h->session == p->session && p->sequence <= h->sequence)
        return 1;
    *h = (remote_history_t){p->device_id, p->session, p->sequence};
    return 0;
}
