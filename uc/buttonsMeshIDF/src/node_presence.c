/**
 * @file node_presence.c
 * @brief Presence sensor node: Waveshare HMMD mmWave radar (SKU 26536,
 *        ICLegend S3KM1110) on an ESP32-C3 SuperMini.
 *
 * The radar reports presence, the target distance and the energy of each
 * 0.7 m range gate ten times a second. Presence changes are sent to the root
 * as button events on three inputs, so RCM wires them like switch buttons:
 *  - 'a' (BUTTON_TYPE_FOLLOW):   outputs on while occupied, off when clear.
 *  - 'b' (BUTTON_TYPE_ON_ONLY):  outputs on when presence starts.
 *  - 'c' (BUTTON_TYPE_OFF_ONLY): outputs off when the area becomes clear.
 *
 * Range gates, absence delay and per-gate thresholds live in the radar's own
 * flash. Turbacz changes them with {"radar": ...} JSON commands; every change
 * is read back from the radar and reported as a {"report":"config"} status.
 * Only this task touches the UART: mesh handlers just queue commands.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "domator_mesh.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "nvs.h"
#include "radar_protocol.h"

static const char* TAG = "PRESENCE";

#define RADAR_UART UART_NUM_1
#define RADAR_REPLY_MS 400
#define RADAR_ATTEMPTS 3
#define RADAR_SILENCE_MS 3000
#define RADAR_RETRY_MS 5000
#define RADAR_STARTUP_MS 3000  // from boot to the first setup
#define PRESENCE_SYNC_MIN_MS 60000
#define LIVE_INTERVAL_MS 1000
#define LIVE_MAX_S 120
#define CALIBRATE_MIN_S 5
#define CALIBRATE_MAX_S 60
#define CALIBRATE_SETTLE_MS 10000  // time to leave the area before measuring
#define CALIBRATE_MIN_SAMPLES 10
#define PRESENCE_INPUTS "abc"

enum { OP_READ, OP_APPLY, OP_FACTORY, OP_CALIBRATE, OP_LIVE, OP_COUNT };
static const char* const OP_NAMES[OP_COUNT] = {"read", "apply", "factory", "calibrate", "live"};

typedef struct {
    uint8_t op;
    uint8_t led;
    uint16_t seconds;
    radar_config_t config;
} presence_cmd_t;

/* Defaults of the vendor's configuration tool (appConfig.xml): 12 gates, a 30 s
 * absence delay and its per-gate thresholds in dB, converted to raw energy. */
static const radar_config_t RADAR_DEFAULTS = {
    .min_gate = 0,
    .max_gate = 12,
    .delay_s = 30,
    .trigger = {60000, 30000, 3000, 2000, 500, 400, 400, 300, 300, 300, 300, 250, 250, 200, 200, 200},
    .hold = {40000, 20000, 400, 300, 300, 200, 200, 150, 150, 100, 100, 100, 100, 100, 100, 100},
};

static QueueHandle_t s_commands;
static radar_parser_t s_parser;
static radar_config_t s_config;
static bool s_config_valid;
static char s_version[12];
static bool s_led;
static volatile bool s_presence, s_radar_ok;
static volatile uint16_t s_distance;
// Highest energy of each gate since the last live report, and while calibrating.
static uint16_t s_live_energy[RADAR_GATES], s_peak[RADAR_GATES];
static uint32_t s_samples;
static int64_t s_last_report, s_sync_at, s_live_until, s_next_live;
static int64_t s_calibrate_from, s_calibrate_until;
static bool s_seen, s_pending, s_calibrating, s_config_pending;
// What arrived since the last report, to explain a failed setup.
static bool s_replied;
static uint32_t s_rx_bytes;
static uint8_t s_rx_sample[16], s_rx_sample_len;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void presence_led_update(void) {
    gpio_set_level(LED_GPIO, s_led && s_presence ? REMOTE_LED_ON_LEVEL : REMOTE_LED_OFF_LEVEL);
}

static void presence_led_store(void) {
    nvs_handle_t handle;
    if (nvs_open("domator", NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u8(handle, "presence_led", s_led) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

// ====================
// Reports to the root
// ====================

static void presence_send_inputs(void) {
    for (const char* input = PRESENCE_INPUTS; *input; input++) {
        mesh_app_msg_t msg = {.src_id = g_device_id, .msg_type = MSG_TYPE_BUTTON};
        msg.data[0] = *input;
        msg.data[1] = s_presence ? '1' : '0';
        msg.data[2] = '0';  // releases carry the short-press flag, like switches
        msg.data_len = s_presence ? 2 : 3;
        mesh_queue_to_node(&msg, TX_PRIO_NORMAL, NULL);
    }
    if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_stats.button_presses++;
        xSemaphoreGive(g_stats_mutex);
    }
    ESP_LOGI(TAG, "Presence %s sent", s_presence ? "detected" : "cleared");
}

static cJSON* presence_report_json(const char* kind) {
    cJSON* json = cJSON_CreateObject();
    if (json) {
        cJSON_AddNumberToObject(json, "deviceId", g_device_id);
        cJSON_AddStringToObject(json, "type", "presence");
        cJSON_AddStringToObject(json, "report", kind);
    }
    return json;
}

/* The root publishes status messages verbatim on /switch/state/root. */
static void presence_send_json(cJSON* json) {
    char* text = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (!text) return;
    size_t len = strlen(text);
    if (len < MESH_MSG_DATA_SIZE) {
        mesh_app_msg_t msg = {.src_id = g_device_id, .msg_type = MSG_TYPE_STATUS, .data_len = len};
        memcpy(msg.data, text, len);
        mesh_queue_to_node(&msg, TX_PRIO_NORMAL, NULL);
    } else {
        ESP_LOGW(TAG, "Report too large (%u bytes)", (unsigned)len);
    }
    free(text);
}

static void presence_send_config(void) {
    cJSON* json = presence_report_json("config");
    if (!json) return;
    cJSON_AddStringToObject(json, "fw", s_version);
    cJSON_AddNumberToObject(json, "min", s_config.min_gate);
    cJSON_AddNumberToObject(json, "max", s_config.max_gate);
    cJSON_AddNumberToObject(json, "delay", s_config.delay_s);
    cJSON_AddNumberToObject(json, "led", s_led);
    cJSON* trigger = cJSON_AddArrayToObject(json, "trigger");
    cJSON* hold = cJSON_AddArrayToObject(json, "hold");
    for (int gate = 0; gate < RADAR_GATES; gate++) {
        cJSON_AddItemToArray(trigger, cJSON_CreateNumber(s_config.trigger[gate]));
        cJSON_AddItemToArray(hold, cJSON_CreateNumber(s_config.hold[gate]));
    }
    presence_send_json(json);
}

static void presence_send_live(int64_t now) {
    cJSON* json = presence_report_json("live");
    if (!json) return;
    cJSON_AddNumberToObject(json, "presence", s_presence);
    cJSON_AddNumberToObject(json, "distance", s_distance);
    if (s_calibrating) cJSON_AddNumberToObject(json, "calibrating", (s_calibrate_until - now + 999) / 1000);
    cJSON* energy = cJSON_AddArrayToObject(json, "energy");
    for (int gate = 0; gate < RADAR_GATES; gate++)
        cJSON_AddItemToArray(energy, cJSON_CreateNumber(s_live_energy[gate]));
    memset(s_live_energy, 0, sizeof(s_live_energy));
    presence_send_json(json);
}

void presence_add_status(cJSON* json) {
    cJSON_AddNumberToObject(json, "presence", s_presence);
    cJSON_AddNumberToObject(json, "distance", s_distance);
    cJSON_AddNumberToObject(json, "radar", s_radar_ok);
}

// ====================
// Radar UART
// ====================

static void presence_report(const radar_report_t* report) {
    int64_t now = now_ms();
    s_last_report = now;
    s_radar_ok = true;
    s_rx_bytes = s_rx_sample_len = 0;
    s_distance = report->distance_cm;
    // The radar decides on every report: a live view of one report a second
    // would miss the peaks that start presence.
    for (int gate = 0; gate < RADAR_GATES; gate++)
        if (report->energy[gate] > s_live_energy[gate]) s_live_energy[gate] = report->energy[gate];
    if (s_calibrating && now >= s_calibrate_from) {
        s_samples++;
        for (int gate = 0; gate < RADAR_GATES; gate++)
            if (report->energy[gate] > s_peak[gate]) s_peak[gate] = report->energy[gate];
    }
    if (!s_seen) {
        // The first state after boot is sent only once it has lasted the
        // absence delay: a person sitting still may not be detected at once,
        // and lights must not flash off because the sensor restarted.
        int64_t delay = s_config_valid ? s_config.delay_s * 1000LL : 0;
        s_seen = true;
        s_presence = report->presence;
        s_sync_at = now + (delay > PRESENCE_SYNC_MIN_MS ? delay : PRESENCE_SYNC_MIN_MS);
    } else if (report->presence != s_presence) {
        s_presence = report->presence;
        s_pending = true;
        s_sync_at = 0;
    }
    presence_led_update();
}

/* Handle what arrives within @p wait. Returns true when a reply to
 * @p command was copied to @p reply. Reports are processed throughout. */
static bool radar_poll(TickType_t wait, int command, radar_reply_t* reply) {
    uint8_t chunk[64];
    int received = uart_read_bytes(RADAR_UART, chunk, sizeof(chunk), wait);
    bool replied = false;
    if (received > 0) s_rx_bytes += received;
    for (int i = 0; i < received; i++) {
        if (s_rx_sample_len < sizeof(s_rx_sample)) s_rx_sample[s_rx_sample_len++] = chunk[i];
        radar_report_t report;
        radar_reply_t frame;
        int kind = radar_parse(&s_parser, chunk[i], &report, &frame);
        if (kind == RADAR_REPORT) {
            presence_report(&report);
        } else if (kind == RADAR_REPLY && frame.command == command && reply) {
            *reply = frame;
            replied = true;
        }
    }
    return replied;
}

static bool radar_request(uint16_t command, const uint8_t* data, size_t len, radar_reply_t* reply) {
    uint8_t frame[RADAR_COMMAND_MAX];
    size_t size = radar_command(frame, command, data, len);
    radar_reply_t ignored;
    if (!reply) reply = &ignored;
    // The module often misses the first command while it is streaming.
    for (int attempt = 0; size && attempt < RADAR_ATTEMPTS; attempt++) {
        uart_write_bytes(RADAR_UART, frame, size);
        int64_t deadline = now_ms() + RADAR_REPLY_MS;
        while (now_ms() < deadline) {
            if (!radar_poll(pdMS_TO_TICKS(20), command, reply)) continue;
            s_replied = true;
            if (!reply->status) return true;
            ESP_LOGW(TAG, "Radar rejected command 0x%04x (status %u)", command, reply->status);
            return false;
        }
    }
    ESP_LOGW(TAG, "No radar reply to command 0x%04x", command);
    return false;
}

static void radar_read_version(void) {
    radar_reply_t reply;
    if (!radar_request(RADAR_CMD_READ_VERSION, NULL, 0, &reply) || reply.data_len < 2) return;
    size_t len = radar_u16(reply.data);
    if (len > reply.data_len - 2u) len = reply.data_len - 2u;
    if (len >= sizeof(s_version)) len = sizeof(s_version) - 1;
    for (size_t i = 0; i < len; i++) {
        char c = reply.data[2 + i];
        s_version[i] = c >= ' ' && c <= '~' ? c : '?';
    }
    s_version[len] = 0;
}

static bool radar_read_config(radar_config_t* config) {
    // Four values per reply keep it within the module's 32-byte buffer.
    for (int first = 0; first < RADAR_PARAMS; first += 4) {
        int count = RADAR_PARAMS - first < 4 ? RADAR_PARAMS - first : 4;
        uint8_t ids[8];
        radar_reply_t reply;
        for (int i = 0; i < count; i++) radar_put16(ids + 2 * i, radar_param_id(first + i));
        if (!radar_request(RADAR_CMD_READ_PARAMS, ids, 2 * count, &reply) || reply.data_len < 4 * count)
            return false;
        for (int i = 0; i < count; i++) *radar_param(config, first + i) = radar_u32(reply.data + 4 * i);
    }
    return true;
}

static bool radar_write_config(radar_config_t* want) {
    uint8_t data[18];  // three parameters per command
    size_t len = 0;
    bool ok = true;
    for (int i = 0; i < RADAR_PARAMS; i++) {
        uint32_t value = *radar_param(want, i);
        if (s_config_valid && value == *radar_param(&s_config, i)) continue;
        radar_put16(data + len, radar_param_id(i));
        radar_put32(data + len + 2, value);
        len += 6;
        if (len == sizeof(data)) {
            ok = radar_request(RADAR_CMD_WRITE_PARAMS, data, len, NULL) && ok;
            len = 0;
        }
    }
    if (len) ok = radar_request(RADAR_CMD_WRITE_PARAMS, data, len, NULL) && ok;
    return ok;
}

/* One configuration session: optionally write @p want, read everything back
 * and select energy reports. Leaving configuration mode stores written values
 * in the radar's flash. */
static bool radar_session(radar_config_t* want) {
    uint8_t version[2];
    s_replied = false;
    radar_put16(version, RADAR_PROTOCOL_VERSION);
    if (!radar_request(RADAR_CMD_ENABLE_CONFIG, version, sizeof(version), NULL)) return false;
    bool ok = !want || radar_write_config(want);
    radar_read_version();
    radar_config_t config;
    if (radar_read_config(&config)) {
        s_config = config;
        s_config_valid = true;
    } else {
        ok = false;
    }
    uint8_t mode[6];
    radar_put16(mode, RADAR_SYSTEM_MODE);
    radar_put32(mode + 2, RADAR_MODE_REPORT);
    ok = radar_request(RADAR_CMD_SET_SYSTEM, mode, sizeof(mode), NULL) && ok;
    ok = radar_request(RADAR_CMD_DISABLE_CONFIG, NULL, 0, NULL) && ok;
    s_last_report = now_ms();  // reports resume after configuration mode
    return ok;
}

/* Explain a failed setup by what the board received instead of replies: a
 * silent line points at the radar's TX wire or its power, and the radar's
 * ON/OFF text alone at its RX wire or a locked-up radar. */
static void radar_log_failure(void) {
    char text[sizeof(s_rx_sample) + 1], hex[3 * sizeof(s_rx_sample) + 1] = "";
    int n = s_rx_sample_len;
    bool is_text = n > 0;
    for (int i = 0; i < n; i++) {
        uint8_t c = s_rx_sample[i];
        bool shown = c >= ' ' && c <= '~';
        is_text = is_text && (shown || c == '\r' || c == '\n');
        text[i] = shown ? c : '.';
        snprintf(hex + 3 * i, 4, "%02X ", c);
    }
    text[n] = 0;
    if (n) hex[3 * n - 1] = 0;
    if (s_replied)
        ESP_LOGW(TAG, "Radar setup incomplete: the radar answered only some commands");
    else if (!s_rx_bytes)
        ESP_LOGW(TAG, "Radar setup incomplete: nothing received on GPIO%d. Check radar TX -> board RX, "
                      "and that the radar has 3V3 and GND", PRESENCE_RADAR_RX_GPIO);
    else if (is_text)
        ESP_LOGW(TAG, "Radar setup incomplete: the radar sends \"%s\" but ignores commands. "
                      "Check GPIO%d -> radar RX, or power the radar off for a few seconds",
                 text, PRESENCE_RADAR_TX_GPIO);
    else
        ESP_LOGW(TAG, "Radar setup incomplete: no reply in %" PRIu32 " bytes received, starting %s",
                 s_rx_bytes, hex);
    s_rx_bytes = s_rx_sample_len = 0;
}

// ====================
// Commands
// ====================

static bool json_number(const cJSON* item, uint32_t max, uint32_t* out) {
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > max) return false;
    *out = (uint32_t)item->valuedouble;
    return *out == item->valuedouble;
}

static bool json_gates(const cJSON* array, uint32_t* out) {
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != RADAR_GATES) return false;
    int gate = 0;
    const cJSON* item;
    cJSON_ArrayForEach(item, array) {
        if (!json_number(item, UINT32_MAX, &out[gate++])) return false;
    }
    return true;
}

bool presence_handle_command(const char* data, size_t len) {
    if (g_node_type != NODE_TYPE_PRESENCE || !len || data[0] != '{') return false;
    cJSON* json = cJSON_ParseWithLength(data, len);
    const cJSON* op = cJSON_GetObjectItem(json, "radar");
    if (!cJSON_IsString(op)) {
        cJSON_Delete(json);
        return false;
    }
    presence_cmd_t cmd = {.op = OP_COUNT};
    for (int i = 0; i < OP_COUNT; i++)
        if (!strcmp(op->valuestring, OP_NAMES[i])) cmd.op = i;
    uint32_t value = 0;
    bool valid = cmd.op != OP_COUNT;
    if (cmd.op == OP_APPLY) {
        radar_config_t* c = &cmd.config;
        valid = json_number(cJSON_GetObjectItem(json, "min"), RADAR_GATES - 1, &c->min_gate) &&
                json_number(cJSON_GetObjectItem(json, "max"), RADAR_GATES - 1, &c->max_gate) &&
                c->min_gate <= c->max_gate &&
                json_number(cJSON_GetObjectItem(json, "delay"), UINT16_MAX, &c->delay_s) &&
                json_gates(cJSON_GetObjectItem(json, "trigger"), c->trigger) &&
                json_gates(cJSON_GetObjectItem(json, "hold"), c->hold) &&
                json_number(cJSON_GetObjectItem(json, "led"), 1, &value);
        cmd.led = value;
    } else if (cmd.op == OP_CALIBRATE || cmd.op == OP_LIVE) {
        uint32_t low = cmd.op == OP_CALIBRATE ? CALIBRATE_MIN_S : 0;
        uint32_t high = cmd.op == OP_CALIBRATE ? CALIBRATE_MAX_S : LIVE_MAX_S;
        valid = json_number(cJSON_GetObjectItem(json, "seconds"), high, &value) && value >= low;
        cmd.seconds = value;
    }
    cJSON_Delete(json);
    if (!valid) {
        ESP_LOGW(TAG, "Ignoring invalid radar command");
    } else if (!s_commands || xQueueSend(s_commands, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Radar busy; command dropped");
    }
    return true;
}

static void presence_calibrated(void) {
    s_calibrating = false;
    if (s_samples < CALIBRATE_MIN_SAMPLES) {
        ESP_LOGW(TAG, "Calibration discarded: only %" PRIu32 " radar reports", s_samples);
        return;
    }
    radar_config_t want = s_config;
    for (int gate = 0; gate < RADAR_GATES; gate++) {
        want.trigger[gate] = radar_threshold(s_peak[gate], RADAR_TRIGGER_MARGIN_X1000, RADAR_TRIGGER_FLOOR);
        want.hold[gate] = radar_threshold(s_peak[gate], RADAR_HOLD_MARGIN_X1000, RADAR_HOLD_FLOOR);
    }
    ESP_LOGI(TAG, "Calibrated from %" PRIu32 " reports", s_samples);
    if (radar_session(&want)) s_config_pending = true;
}

/* Stream live data until @p until. A new stream first collects a second of
 * reports, so its first peaks are not left over from long ago. */
static void presence_live_until(int64_t until) {
    int64_t now = now_ms();
    if (now >= s_live_until && until > now) {
        memset(s_live_energy, 0, sizeof(s_live_energy));
        s_next_live = now + LIVE_INTERVAL_MS;
    }
    s_live_until = until;
}

static void presence_run(const presence_cmd_t* cmd) {
    radar_config_t want = cmd->op == OP_FACTORY ? RADAR_DEFAULTS : cmd->config;
    switch (cmd->op) {
        case OP_LIVE:
            presence_live_until(cmd->seconds ? now_ms() + cmd->seconds * 1000LL : 0);
            return;
        case OP_CALIBRATE:
            // Thresholds come from the energy measured while the area is empty.
            if (!s_config_valid) return;
            memset(s_peak, 0, sizeof(s_peak));
            s_samples = 0;
            s_calibrating = true;
            s_calibrate_from = now_ms() + CALIBRATE_SETTLE_MS;
            s_calibrate_until = s_calibrate_from + cmd->seconds * 1000LL;
            if (s_live_until < s_calibrate_until + LIVE_INTERVAL_MS)
                presence_live_until(s_calibrate_until + LIVE_INTERVAL_MS);
            return;
        case OP_APPLY:
            if (s_led != cmd->led) {
                s_led = cmd->led;
                presence_led_store();
                presence_led_update();
            }
            // fall through
        case OP_FACTORY:
            if (radar_session(&want)) s_config_pending = true;
            return;
        default:
            if (radar_session(NULL)) s_config_pending = true;
    }
}

// ====================
// Initialisation and task
// ====================

void presence_init(void) {
    s_commands = xQueueCreate(4, sizeof(presence_cmd_t));
    configASSERT(s_commands);

    uint8_t led = 0;
    nvs_handle_t handle;
    if (nvs_open("domator", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "presence_led", &led);
        nvs_close(handle);
    }
    s_led = led == 1;
    gpio_config_t led_pin = {.pin_bit_mask = 1ULL << LED_GPIO, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&led_pin));
    presence_led_update();

    const uart_config_t uart = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(RADAR_UART, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(RADAR_UART, &uart));
    // RX takes over the console's input pin, which nothing reads. The console
    // keeps its TX pin, whose boot log must not reach the radar.
    ESP_ERROR_CHECK(uart_set_pin(RADAR_UART, PRESENCE_RADAR_TX_GPIO, PRESENCE_RADAR_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // An unconnected sensor must read as an idle line, not noise.
    gpio_set_pull_mode(PRESENCE_RADAR_RX_GPIO, GPIO_PULLUP_ONLY);
    ESP_LOGI(TAG, "HMMD radar on GPIO%d (RX) / GPIO%d (TX), LED %s", PRESENCE_RADAR_RX_GPIO,
             PRESENCE_RADAR_TX_GPIO, s_led ? "on" : "off");
}

void presence_task(void* arg) {
    bool ready = false;
    // The radar powers on with the board: give it time to start before the
    // first command. Whatever it sends meanwhile is still read.
    int64_t retry_at = RADAR_STARTUP_MS;
    while (true) {
        if (g_ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!ready && now_ms() >= retry_at) {
            if (radar_session(NULL)) s_config_pending = true;
            else radar_log_failure();
            // Retry only when reports stop: a failed read alone must not
            // interrupt detection with a configuration session every 5 s.
            ready = true;
            retry_at = now_ms() + RADAR_RETRY_MS;
        }
        radar_poll(pdMS_TO_TICKS(20), -1, NULL);

        int64_t now = now_ms();
        if (now - s_last_report > RADAR_SILENCE_MS) {
            if (s_radar_ok) ESP_LOGW(TAG, "Radar stopped reporting");
            s_radar_ok = false;
            ready = false;
        }
        presence_cmd_t cmd;
        if (xQueueReceive(s_commands, &cmd, 0) == pdTRUE) presence_run(&cmd);
        if (s_calibrating && now >= s_calibrate_until) presence_calibrated();
        if (s_sync_at && now >= s_sync_at) {
            s_pending = true;
            s_sync_at = 0;
        }
        // Undelivered changes wait for the mesh; only the latest state is sent.
        if (s_pending && g_mesh_connected) {
            s_pending = false;
            presence_send_inputs();
        }
        // The radar is set up before the mesh, and a root publishes status
        // reports only while MQTT is connected.
        if (s_config_pending && g_mesh_connected && (!g_is_root || g_mqtt_connected)) {
            s_config_pending = false;
            presence_send_config();
        }
        if (now < s_live_until && now >= s_next_live && g_mesh_connected) {
            s_next_live = now + LIVE_INTERVAL_MS;
            presence_send_live(now);
        }
    }
}
