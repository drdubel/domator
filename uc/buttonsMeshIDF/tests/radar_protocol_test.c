#include <assert.h>
#include <string.h>
#include "radar_protocol.h"

static int feed(radar_parser_t* p, const uint8_t* bytes, size_t len, radar_report_t* report,
                radar_reply_t* reply, int* frames) {
    int last = RADAR_NONE;
    for (size_t i = 0; i < len; i++) {
        int kind = radar_parse(p, bytes[i], report, reply);
        if (kind != RADAR_NONE) {
            last = kind;
            (*frames)++;
        }
    }
    return last;
}

static size_t report_frame(uint8_t* out, uint8_t presence, uint16_t distance, uint16_t energy) {
    memcpy(out, RADAR_REPORT_HEADER, 4);
    radar_put16(out + 4, RADAR_REPORT_LEN);
    out[6] = presence;
    radar_put16(out + 7, distance);
    for (int gate = 0; gate < RADAR_GATES; gate++) radar_put16(out + 9 + 2 * gate, energy + gate);
    memcpy(out + 41, RADAR_REPORT_FOOTER, 4);
    return 45;
}

int main(void) {
    // The module's documented "read firmware version" request.
    uint8_t frame[RADAR_COMMAND_MAX];
    const uint8_t version_request[] = {0xFD, 0xFC, 0xFB, 0xFA, 0x02, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01};
    assert(radar_command(frame, RADAR_CMD_READ_VERSION, NULL, 0) == sizeof(version_request));
    assert(!memcmp(frame, version_request, sizeof(version_request)));
    // Report mode, as sent by the Waveshare demo for normal mode (0x64).
    uint8_t mode[6];
    radar_put16(mode, RADAR_SYSTEM_MODE);
    radar_put32(mode + 2, RADAR_MODE_REPORT);
    const uint8_t mode_request[] = {0xFD, 0xFC, 0xFB, 0xFA, 0x08, 0x00, 0x12, 0x00, 0x00, 0x00,
                                    0x04, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01};
    assert(radar_command(frame, RADAR_CMD_SET_SYSTEM, mode, sizeof(mode)) == sizeof(mode_request));
    assert(!memcmp(frame, mode_request, sizeof(mode_request)));
    // Three parameters fill the module's 32-byte buffer; four do not fit.
    uint8_t params[24] = {0};
    assert(radar_command(frame, RADAR_CMD_WRITE_PARAMS, params, 18) == 30);
    assert(radar_command(frame, RADAR_CMD_WRITE_PARAMS, params, 24) == 0);

    radar_parser_t parser = {0};
    radar_report_t report = {0};
    radar_reply_t reply = {0};
    int frames = 0;

    // Version reply from the Waveshare documentation: "v1.5.5".
    const uint8_t version_reply[] = {0xFD, 0xFC, 0xFB, 0xFA, 0x0C, 0x00, 0x00, 0x01, 0x00, 0x00, 0x06, 0x00,
                                     'v', '1', '.', '5', '.', '5', 0x04, 0x03, 0x02, 0x01};
    assert(feed(&parser, version_reply, sizeof(version_reply), &report, &reply, &frames) == RADAR_REPLY);
    assert(frames == 1 && reply.command == RADAR_CMD_READ_VERSION && reply.status == 0);
    assert(reply.data_len == 8 && radar_u16(reply.data) == 6 && !memcmp(reply.data + 2, "v1.5.5", 6));

    // Parameter reply: maximum gate 12. Enable-config reply: protocol 2, buffer 32.
    const uint8_t param_reply[] = {0xFD, 0xFC, 0xFB, 0xFA, 0x08, 0x00, 0x08, 0x01, 0x00, 0x00,
                                   0x0C, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01};
    frames = 0;
    assert(feed(&parser, param_reply, sizeof(param_reply), &report, &reply, &frames) == RADAR_REPLY);
    assert(reply.command == RADAR_CMD_READ_PARAMS && radar_u32(reply.data) == 12);
    const uint8_t enable_reply[] = {0xFD, 0xFC, 0xFB, 0xFA, 0x08, 0x00, 0xFF, 0x01, 0x00, 0x00,
                                    0x02, 0x00, 0x20, 0x00, 0x04, 0x03, 0x02, 0x01};
    assert(feed(&parser, enable_reply, sizeof(enable_reply), &report, &reply, &frames) == RADAR_REPLY);
    assert(reply.command == RADAR_CMD_ENABLE_CONFIG && radar_u16(reply.data + 2) == 32);

    // A failed command keeps its status for the caller.
    uint8_t failed[sizeof(param_reply)];
    memcpy(failed, param_reply, sizeof(failed));
    failed[8] = 1;
    assert(feed(&parser, failed, sizeof(failed), &report, &reply, &frames) == RADAR_REPLY);
    assert(reply.status == 1);

    // Reports, including ones split across reads and preceded by normal-mode text.
    uint8_t stream[200];
    size_t len = 0;
    memcpy(stream, "ON\r\nRange 85\r\n", 14);
    len += 14;
    len += report_frame(stream + len, 1, 140, 300);
    frames = 0;
    assert(feed(&parser, stream, 20, &report, &reply, &frames) == RADAR_NONE && frames == 0);
    assert(feed(&parser, stream + 20, len - 20, &report, &reply, &frames) == RADAR_REPORT && frames == 1);
    assert(report.presence && report.distance_cm == 140);
    assert(report.energy[0] == 300 && report.energy[15] == 315);

    // Gate energy that looks like a footer cannot end the report early.
    len = report_frame(stream, 0, 0, 0);
    memcpy(stream + 9, RADAR_REPORT_FOOTER, 4);
    frames = 0;
    assert(feed(&parser, stream, len, &report, &reply, &frames) == RADAR_REPORT && frames == 1);
    assert(!report.presence && report.energy[0] == 0xF7F8 && report.energy[2] == 2);

    // A corrupted footer is dropped and the next frame is still found.
    len = report_frame(stream, 1, 70, 10);
    stream[len - 1] = 0;
    len += report_frame(stream + len, 0, 210, 20);
    frames = 0;
    assert(feed(&parser, stream, len, &report, &reply, &frames) == RADAR_REPORT && frames == 1);
    assert(!report.presence && report.distance_cm == 210);

    // Impossible lengths and a request echoed back are not frames.
    uint8_t bad[sizeof(param_reply)];
    memcpy(bad, param_reply, sizeof(bad));
    bad[4] = 0xFF;
    frames = 0;
    feed(&parser, bad, sizeof(bad), &report, &reply, &frames);
    assert(frames == 0);
    feed(&parser, version_request, sizeof(version_request), &report, &reply, &frames);
    assert(frames == 0);
    len = report_frame(stream, 1, 1, 1);
    radar_put16(stream + 4, RADAR_REPORT_LEN + 1);
    feed(&parser, stream, len, &report, &reply, &frames);
    assert(frames == 0);
    // Garbage of any value never overflows the buffer or blocks resynchronisation.
    for (int value = 0; value < 4096; value++) {
        uint8_t byte = (uint8_t)(value * 37);
        radar_parse(&parser, byte, &report, &reply);
        assert(parser.len <= sizeof(parser.buf));
    }
    len = report_frame(stream, 1, 350, 5);
    frames = 0;
    feed(&parser, stream, len, &report, &reply, &frames);
    assert(frames >= 1 && report.distance_cm == 350);

    // Parameter table order matches the protocol IDs.
    radar_config_t config = {0};
    assert(radar_param_id(0) == RADAR_PARAM_MIN_GATE && radar_param(&config, 0) == &config.min_gate);
    assert(radar_param_id(2) == RADAR_PARAM_DELAY && radar_param(&config, 2) == &config.delay_s);
    assert(radar_param_id(3) == 0x10 && radar_param(&config, 3) == &config.trigger[0]);
    assert(radar_param_id(18) == 0x1F && radar_param(&config, 18) == &config.trigger[15]);
    assert(radar_param_id(19) == 0x20 && radar_param(&config, 19) == &config.hold[0]);
    assert(radar_param_id(RADAR_PARAMS - 1) == 0x2F && radar_param(&config, RADAR_PARAMS - 1) == &config.hold[15]);

    // Calibration: +5.0 dB / +3.5 dB above the peak, with floors and saturation.
    assert(radar_threshold(10000, RADAR_TRIGGER_MARGIN_X1000, RADAR_TRIGGER_FLOOR) == 31620);
    assert(radar_threshold(10000, RADAR_HOLD_MARGIN_X1000, RADAR_HOLD_FLOOR) == 22390);
    assert(radar_threshold(0, RADAR_TRIGGER_MARGIN_X1000, RADAR_TRIGGER_FLOOR) == RADAR_TRIGGER_FLOOR);
    assert(radar_threshold(UINT32_MAX, RADAR_TRIGGER_MARGIN_X1000, RADAR_TRIGGER_FLOOR) == UINT32_MAX);
    return 0;
}
