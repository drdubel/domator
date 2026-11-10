#pragma once
/* Waveshare HMMD mmWave sensor (SKU 26536, ICLegend S3KM1110) UART protocol.
 *
 * Commands:  FD FC FB FA | len u16 | cmd u16 | data | 04 03 02 01
 * Replies:   same framing; cmd has 0x0100 set and data starts with a u16
 *            status (0 = success).
 * Reports:   F4 F3 F2 F1 | 35 u16 | presence u8 | distance u16 (cm) |
 *            16 x u16 gate energy | F8 F7 F6 F5, ten per second.
 * All integers are little endian. The module announces a 32-byte buffer when
 * configuration mode is enabled, so no command frame may exceed that.
 *
 * No IDF dependency: framing and calibration are checked on the host. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RADAR_GATES 16
#define RADAR_GATE_CM 70
#define RADAR_COMMAND_MAX 32
#define RADAR_FRAME_MAX 64
#define RADAR_REPORT_LEN 35

#define RADAR_CMD_READ_VERSION 0x0000
#define RADAR_CMD_WRITE_PARAMS 0x0007
#define RADAR_CMD_READ_PARAMS 0x0008
#define RADAR_CMD_SET_SYSTEM 0x0012
#define RADAR_CMD_DISABLE_CONFIG 0x00FE
#define RADAR_CMD_ENABLE_CONFIG 0x00FF
#define RADAR_PROTOCOL_VERSION 2

#define RADAR_PARAM_MIN_GATE 0x0000
#define RADAR_PARAM_MAX_GATE 0x0001
#define RADAR_PARAM_DELAY 0x0004
#define RADAR_PARAM_TRIGGER 0x0010 /* + gate: energy that starts presence */
#define RADAR_PARAM_HOLD 0x0020    /* + gate: energy that keeps presence */
#define RADAR_SYSTEM_MODE 0x0000
#define RADAR_MODE_REPORT 0x00000004u

/* The vendor tool calibrates trigger thresholds 5.0 dB and hold thresholds
 * 3.5 dB above the empty-room peak. Floors match its least sensitive gates. */
#define RADAR_TRIGGER_MARGIN_X1000 3162u
#define RADAR_HOLD_MARGIN_X1000 2239u
#define RADAR_TRIGGER_FLOOR 200u
#define RADAR_HOLD_FLOOR 100u

/* Every writable parameter, in a fixed order: min, max, delay, 16 trigger
 * and 16 hold thresholds. */
#define RADAR_PARAMS (3 + 2 * RADAR_GATES)

typedef struct {
    uint32_t min_gate, max_gate, delay_s;
    uint32_t trigger[RADAR_GATES], hold[RADAR_GATES];
} radar_config_t;

typedef struct {
    bool presence;
    uint16_t distance_cm;
    uint16_t energy[RADAR_GATES];
} radar_report_t;

typedef struct {
    uint16_t command; /* request code, without the reply flag */
    uint16_t status;
    uint8_t data[RADAR_FRAME_MAX];
    uint8_t data_len;
} radar_reply_t;

typedef struct {
    uint8_t buf[RADAR_FRAME_MAX];
    uint8_t len;
} radar_parser_t;

enum { RADAR_NONE, RADAR_REPORT, RADAR_REPLY };

static const uint8_t RADAR_CMD_HEADER[4] = {0xFD, 0xFC, 0xFB, 0xFA};
static const uint8_t RADAR_CMD_FOOTER[4] = {0x04, 0x03, 0x02, 0x01};
static const uint8_t RADAR_REPORT_HEADER[4] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t RADAR_REPORT_FOOTER[4] = {0xF8, 0xF7, 0xF6, 0xF5};

static inline uint16_t radar_u16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

static inline uint32_t radar_u32(const uint8_t* p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void radar_put16(uint8_t* p, uint32_t value) {
    p[0] = value;
    p[1] = value >> 8;
}

static inline void radar_put32(uint8_t* p, uint32_t value) {
    radar_put16(p, value);
    radar_put16(p + 2, value >> 16);
}

/* Returns the frame size, or 0 when it would overflow the module's buffer. */
static inline size_t radar_command(uint8_t* out, uint16_t command, const uint8_t* data, size_t len) {
    size_t size = 4 + 2 + 2 + len + 4;
    if (size > RADAR_COMMAND_MAX) return 0;
    memcpy(out, RADAR_CMD_HEADER, 4);
    radar_put16(out + 4, len + 2);
    radar_put16(out + 6, command);
    if (len) memcpy(out + 8, data, len);
    memcpy(out + 8 + len, RADAR_CMD_FOOTER, 4);
    return size;
}

static inline uint16_t radar_param_id(int index) {
    if (index == 0) return RADAR_PARAM_MIN_GATE;
    if (index == 1) return RADAR_PARAM_MAX_GATE;
    if (index == 2) return RADAR_PARAM_DELAY;
    if (index < 3 + RADAR_GATES) return RADAR_PARAM_TRIGGER + index - 3;
    return RADAR_PARAM_HOLD + index - 3 - RADAR_GATES;
}

static inline uint32_t* radar_param(radar_config_t* config, int index) {
    if (index == 0) return &config->min_gate;
    if (index == 1) return &config->max_gate;
    if (index == 2) return &config->delay_s;
    if (index < 3 + RADAR_GATES) return &config->trigger[index - 3];
    return &config->hold[index - 3 - RADAR_GATES];
}

/* Whether the buffered bytes can still begin a frame. */
static inline bool radar_header_prefix(const radar_parser_t* p) {
    size_t n = p->len < 4 ? p->len : 4;
    return !memcmp(p->buf, RADAR_CMD_HEADER, n) || !memcmp(p->buf, RADAR_REPORT_HEADER, n);
}

/* Feed one received byte. Frames are delimited by their length field, so gate
 * energies that resemble a footer cannot end a report early. A malformed frame
 * discards only its first byte before searching for the next header, and the
 * module's plain-text "ON"/"OFF" output in normal mode is skipped. */
static inline int radar_parse(radar_parser_t* p, uint8_t byte, radar_report_t* report,
                              radar_reply_t* reply) {
    if (p->len >= sizeof(p->buf)) p->len = 0;
    p->buf[p->len++] = byte;
    while (p->len) {
        bool valid = radar_header_prefix(p);
        if (valid && p->len >= 6) {
            bool is_report = p->buf[0] == RADAR_REPORT_HEADER[0];
            size_t body = radar_u16(p->buf + 4);
            valid = is_report ? body == RADAR_REPORT_LEN : body >= 4 && body + 10 <= sizeof(p->buf);
            if (valid && p->len == body + 10) {
                const uint8_t* footer = is_report ? RADAR_REPORT_FOOTER : RADAR_CMD_FOOTER;
                // Replies carry the request code with 0x0100 set.
                valid = !memcmp(p->buf + 6 + body, footer, 4) && (is_report || p->buf[7] == 0x01);
                if (valid) {
                    p->len = 0;
                    if (is_report) {
                        report->presence = p->buf[6] == 1;
                        report->distance_cm = radar_u16(p->buf + 7);
                        for (int gate = 0; gate < RADAR_GATES; gate++)
                            report->energy[gate] = radar_u16(p->buf + 9 + 2 * gate);
                        return RADAR_REPORT;
                    }
                    reply->command = radar_u16(p->buf + 6) & 0x00FF;
                    reply->status = radar_u16(p->buf + 8);
                    reply->data_len = body - 4;
                    memcpy(reply->data, p->buf + 10, reply->data_len);
                    return RADAR_REPLY;
                }
            }
        }
        if (valid) return RADAR_NONE;
        memmove(p->buf, p->buf + 1, --p->len);
    }
    return RADAR_NONE;
}

/* Calibrated threshold for an empty-room peak, saturating at UINT32_MAX. */
static inline uint32_t radar_threshold(uint32_t peak, uint32_t margin_x1000, uint32_t floor) {
    uint64_t value = (uint64_t)peak * margin_x1000 / 1000;
    if (value < floor) value = floor;
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}
