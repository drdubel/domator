/** ESP-NOW switch gateway and C3 remote lifecycle.
 * Normal nodes do not initialise ESP-NOW unless explicitly enabled.
 * Radio callbacks only copy packets. Mesh/radio/NVS work runs in tasks.
 */
#include "domator_mesh.h"
#include "remote_protocol.h"
#include "driver/gpio.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include <stdlib.h>
#include <ctype.h>

static const char* TAG = "REMOTE";
static bool gateway_enabled;
static SemaphoreHandle_t gateway_lock;
static bool now_started;
static uint8_t auth_key[32];
static bool key_ready;
#ifdef CONFIG_IDF_TARGET_ESP32C3
static uint8_t gateway_mac[6];
#endif
static QueueHandle_t now_rx;
static QueueHandle_t mesh_down;
static TaskHandle_t gateway_task_handle;
static uint32_t command_session, command_sequence;

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    remote_packet_t packet;
} received_t;

typedef struct {
    uint64_t id;
    uint8_t mac[6];
    int64_t seen;
    bool pending;
    remote_packet_t command;
} gateway_peer_t;
static gateway_peer_t* peers;

static remote_history_t root_history[MAX_NODES];

static bool read_key(void) {
    if (key_ready) return true;
    const char* value = credentials_get()->remote_key;
    if (strlen(value) != 64) return false;
    for (int i = 0; i < 64; i++) if (!isxdigit((unsigned char)value[i])) return false;
    for (int i = 0; i < 32; i++) {
        char byte[3] = {value[2*i], value[2*i+1], 0};
        char* end;
        unsigned long n = strtoul(byte, &end, 16);
        if (*end || end != byte + 2) return false;
        auth_key[i] = n;
    }
    key_ready = true;
    return true;
}

static void sign_packet(remote_packet_t* p) {
    uint8_t hash[32];
    ESP_ERROR_CHECK(mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
        auth_key, sizeof(auth_key), (uint8_t*)p, offsetof(remote_packet_t, tag), hash));
    memcpy(p->tag, hash, sizeof(p->tag));
}

static bool authentic(const remote_packet_t* p) {
    if (!remote_packet_shape_valid(p)) return false;
    remote_packet_t copy = *p;
    sign_packet(&copy);
    uint8_t diff = 0;
    for (int i = 0; i < sizeof(p->tag); i++) diff |= copy.tag[i] ^ p->tag[i];
    return diff == 0;
}

static void receive_now(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (!now_rx || len != sizeof(remote_packet_t)) return;
    received_t rx = {0};
    memcpy(rx.mac, info->src_addr, 6);
    if (info->rx_ctrl) rx.rssi = info->rx_ctrl->rssi;
    memcpy(&rx.packet, data, sizeof(rx.packet));
    xQueueSend(now_rx, &rx, 0);
}

static esp_err_t start_now(void) {
    if (now_started) return ESP_OK;
    if (!read_key()) return ESP_ERR_INVALID_ARG;
    if (!now_rx) now_rx = xQueueCreate(16, sizeof(received_t));
    if (!now_rx) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) return err;
    err = esp_now_register_recv_cb(receive_now);
    if (err != ESP_OK) {
        esp_now_deinit();
        return err;
    }
    now_started = true;
    return ESP_OK;
}

static bool send_now(const uint8_t* mac, remote_packet_t* p) {
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_num_t count;
        if (esp_now_get_peer_num(&count) == ESP_OK && count.total_num >= ESP_NOW_MAX_TOTAL_PEER_NUM) {
            esp_now_peer_info_t oldest;
            if (esp_now_fetch_peer(true, &oldest) == ESP_OK) esp_now_del_peer(oldest.peer_addr);
        }
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, mac, 6);
        peer.ifidx = WIFI_IF_STA;
        peer.channel = 0; // Mesh owns the gateway channel.
        if (esp_now_add_peer(&peer) != ESP_OK) return false;
    }
    sign_packet(p);
    return esp_now_send(mac, (uint8_t*)p, sizeof(*p)) == ESP_OK;
}

static bool queue_packet(mesh_addr_t* dest, remote_packet_t* p) {
    mesh_app_msg_t msg = {0};
    msg.src_id = g_device_id;
    msg.msg_type = MSG_TYPE_ESPNOW;
    msg.data_len = sizeof(*p);
    memcpy(msg.data, p, sizeof(*p));
    // FIFO order matters for press/release and the sequence watermark.
    return mesh_queue_to_node(&msg, TX_PRIO_NORMAL, dest);
}

static gateway_peer_t* find_peer(uint64_t id) {
    if (!peers) return NULL;
    for (int i = 0; i < MAX_NODES; i++) if (peers[i].id == id) return &peers[i];
    return NULL;
}

static void gateway_task(void* unused) {
    received_t rx;
    remote_packet_t down;
    while (true) {
        xSemaphoreTake(gateway_lock, portMAX_DELAY);
        if (!gateway_enabled || g_ota_in_progress) {
            xSemaphoreGive(gateway_lock);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        while (xQueueReceive(mesh_down, &down, 0) == pdTRUE) {
            gateway_peer_t* peer = find_peer(down.device_id);
            if (!peer) continue;
            if (down.kind == REMOTE_COMMAND) {
                // Retain until remote acknowledges. Sleeping remotes pick it
                // up on their next event or heartbeat, not via continuous RX.
                peer->command = down;
                peer->pending = true;
            }
            if (peer->pending) send_now(peer->mac, &peer->command);
            if (down.kind == REMOTE_ACK) send_now(peer->mac, &down);
        }
        bool received = xQueueReceive(now_rx, &rx, pdMS_TO_TICKS(2)) == pdTRUE;
        xSemaphoreGive(gateway_lock);
        if (!received) continue;
        xSemaphoreTake(gateway_lock, portMAX_DELAY);
        if (!gateway_enabled) { xSemaphoreGive(gateway_lock); continue; }
        remote_packet_t* p = &rx.packet;
        if (!authentic(p)) { xSemaphoreGive(gateway_lock); continue; }
        gateway_peer_t* peer = find_peer(p->device_id);
        if (p->kind == REMOTE_COMMAND_ACK) {
            if (peer && !memcmp(peer->mac, rx.mac, 6) && peer->pending &&
                peer->command.session == p->session &&
                peer->command.sequence == p->sequence) peer->pending = false;
            { xSemaphoreGive(gateway_lock); continue; }
        }
        if (p->kind != REMOTE_UPLINK || !p->length ||
            (p->payload[0] != MSG_TYPE_BUTTON && p->payload[0] != MSG_TYPE_STATUS &&
             p->payload[0] != MSG_TYPE_PING)) { xSemaphoreGive(gateway_lock); continue; }
        if (!peer) {
            for (int i = 0; i < MAX_NODES; i++) if (!peers[i].id) {
                peer = &peers[i]; peer->id = p->device_id; break;
            }
        }
        if (!peer) { xSemaphoreGive(gateway_lock); continue; }
        memcpy(peer->mac, rx.mac, 6);
        peer->seen = esp_timer_get_time();
        if (peer->pending) send_now(peer->mac, &peer->command);
        // No ACK if disconnected: the remote must know delivery failed.
        if (!g_mesh_connected) { xSemaphoreGive(gateway_lock); continue; }
        if (g_is_root) {
            mesh_app_msg_t msg = {.msg_type = MSG_TYPE_ESPNOW, .data_len = sizeof(*p)};
            memcpy(msg.data, p, sizeof(*p));
            mesh_addr_t self;
            esp_wifi_get_mac(WIFI_IF_STA, self.addr);
            remote_root_receive(&self, &msg);
        } else queue_packet(NULL, p);
        xSemaphoreGive(gateway_lock);
    }
}

bool remote_gateway_enabled(void) { return gateway_enabled; }

void remote_gateway_load(void) {
    read_key();
    do { command_session = esp_random(); } while (!command_session);
    if (g_node_type != NODE_TYPE_SWITCH_C3) return;
    nvs_handle_t h;
    uint8_t enabled = 0;
    if (nvs_open("domator", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "now_gateway", &enabled);
        nvs_close(h);
    }
    gateway_enabled = enabled == 1 && read_key();
    if (enabled && !gateway_enabled) ESP_LOGE(TAG, "Gateway requires remote_key provisioning");
}

void remote_gateway_init(void) {
    if (!gateway_enabled) return;
    if (!gateway_lock) gateway_lock = xSemaphoreCreateMutex();
    configASSERT(gateway_lock);
    esp_err_t err = start_now();
    if (err != ESP_OK) {
        gateway_enabled = false;
        ESP_LOGE(TAG, "ESP-NOW init failed: %s", esp_err_to_name(err));
        return;
    }
    if (!mesh_down) mesh_down = xQueueCreate(16, sizeof(remote_packet_t));
    if (!peers) peers = calloc(MAX_NODES, sizeof(*peers));
    bool ready = mesh_down && peers;
    if (ready && !gateway_task_handle)
        ready = xTaskCreate(gateway_task, "now_gateway", 8192, NULL, 4,
                            &gateway_task_handle) == pdPASS;
    if (!ready) {
        ESP_LOGE(TAG, "Not enough memory to enable gateway");
        gateway_enabled = false;
        esp_now_deinit();
        now_started = false;
        free(peers);
        peers = NULL;
    }
}

bool remote_gateway_command(const char* data, size_t len) {
    if (len != 9 || memcmp(data, "gateway:", 8) || (data[8] != '0' && data[8] != '1'))
        return false;
    if (g_node_type != NODE_TYPE_SWITCH_C3) return true;
    if (!gateway_lock) gateway_lock = xSemaphoreCreateMutex();
    configASSERT(gateway_lock);
    xSemaphoreTake(gateway_lock, portMAX_DELAY);
    bool enabled = data[8] == '1';
    if (enabled && !read_key()) {
        ESP_LOGE(TAG, "Cannot enable gateway without a 64-digit remote_key");
        xSemaphoreGive(gateway_lock);
        return true;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open("domator", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "now_gateway", enabled);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) {
        gateway_enabled = enabled;
        if (enabled) remote_gateway_init();
        else if (now_started) {
            esp_now_deinit();
            now_started = false;
            xQueueReset(now_rx);
            if (mesh_down) xQueueReset(mesh_down);
            free(peers);
            peers = NULL;
        }
        ESP_LOGI(TAG, "Gateway %s", gateway_enabled ? "enabled" : "disabled");
    } else ESP_LOGE(TAG, "Gateway persistence failed: %s", esp_err_to_name(err));
    xSemaphoreGive(gateway_lock);
    return true;
}

void remote_gateway_mesh_receive(mesh_app_msg_t* msg) {
    if (!gateway_enabled || !mesh_down || msg->data_len != sizeof(remote_packet_t)) return;
    remote_packet_t p;
    memcpy(&p, msg->data, sizeof(p));
    if (!remote_packet_shape_valid(&p) ||
        (p.kind != REMOTE_ACK && p.kind != REMOTE_COMMAND)) return;
    xQueueSend(mesh_down, &p, 0);
}

bool remote_wrap_command(uint64_t id, mesh_app_msg_t* msg) {
    if (msg->data_len + 1 > REMOTE_PAYLOAD_SIZE) return false;
    remote_packet_t p = {
        .magic = REMOTE_MAGIC, .version = REMOTE_VERSION, .kind = REMOTE_COMMAND,
        .device_id = id, .length = msg->data_len + 1,
    };
    p.session = command_session;
    static portMUX_TYPE command_lock = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&command_lock);
    p.sequence = ++command_sequence;
    portEXIT_CRITICAL(&command_lock);
    p.payload[0] = msg->msg_type;
    memcpy(p.payload + 1, msg->data, msg->data_len);
    msg->msg_type = MSG_TYPE_ESPNOW;
    msg->data_len = sizeof(p);
    memcpy(msg->data, &p, sizeof(p));
    return true;
}

/* Called only by the mesh RX task, or by a root's gateway worker. */
void remote_root_receive(mesh_addr_t* from, mesh_app_msg_t* msg) {
    if (!node_root_ready() || msg->data_len != sizeof(remote_packet_t)) return;
    remote_packet_t p;
    memcpy(&p, msg->data, sizeof(p));
    // Even a root without gateway mode must authenticate forwarded packets.
    if (!key_ready || !authentic(&p) || !remote_uplink_valid(&p)) return;
    char type = p.payload[0];
    // A mutex also protects a root that itself operates as gateway.
    static portMUX_TYPE history_lock = portMUX_INITIALIZER_UNLOCKED;
    bool duplicate = false, full = true, register_type = false;
    portENTER_CRITICAL(&history_lock);
    for (int i = 0; i < MAX_NODES; i++) {
        remote_history_t* h = &root_history[i];
        if (h->id && h->id != p.device_id) continue;
        full = false;
        register_type = h->id != p.device_id || h->session != p.session;
        duplicate = remote_duplicate(h, &p);
        break;
    }
    portEXIT_CRITICAL(&history_lock);
    if (full) return;
    if (!duplicate) {
        if (register_type) {
            mesh_app_msg_t info = {.src_id = p.device_id, .msg_type = MSG_TYPE_TYPE_INFO,
                                   .data_len = 1, .data = {REMOTE_DEVICE_TYPE}};
            root_handle_mesh_message(from, &info);
        }
        mesh_app_msg_t event = {.src_id = p.device_id, .msg_type = type,
                                .data_len = p.length - 1};
        memcpy(event.data, p.payload + 1, event.data_len);
        root_handle_mesh_message(from, &event);
    }
    p.kind = REMOTE_ACK;
    p.length = 0;
    memset(p.payload, 0, sizeof(p.payload));
    if (gateway_enabled && g_is_root) {
        uint8_t self[6];
        esp_wifi_get_mac(WIFI_IF_STA, self);
        if (!memcmp(from->addr, self, 6)) {
            xQueueSend(mesh_down, &p, 0);
            return;
        }
    }
    queue_packet(from, &p);
}

#ifdef CONFIG_IDF_TARGET_ESP32C3
static uint32_t remote_session, remote_sequence;
static uint32_t last_command_session, last_command_sequence;
static bool radio_running;
static bool ping_pending;
static uint16_t pending_ping;
static int8_t remote_rssi;

static void handle_remote_command(remote_packet_t* p) {
    if (p->kind != REMOTE_COMMAND || !p->length) return;
    bool fresh = p->session != last_command_session || p->sequence > last_command_sequence;
    if (fresh) {
        last_command_session = p->session;
        last_command_sequence = p->sequence;
        if (p->payload[0] == MSG_TYPE_OTA_START) g_ota_requested = true;
        if (p->payload[0] == MSG_TYPE_PING && p->length == 3) {
            memcpy(&pending_ping, p->payload + 1, sizeof(pending_ping));
            ping_pending = true;
        }
    }
    p->kind = REMOTE_COMMAND_ACK;
    p->length = 0;
    memset(p->payload, 0, sizeof(p->payload));
    send_now(gateway_mac, p);
}

static bool remote_send(char type, const void* data, size_t len) {
    if (len + 1 > REMOTE_PAYLOAD_SIZE) return false;
    if (!radio_running) {
        if (esp_wifi_start() != ESP_OK) return false;
        radio_running = true;
        ESP_ERROR_CHECK(esp_wifi_set_channel(credentials_get()->remote_channel,
                                             WIFI_SECOND_CHAN_NONE));
    }
    remote_packet_t packet = {
        .magic = REMOTE_MAGIC, .version = REMOTE_VERSION, .kind = REMOTE_UPLINK,
        .device_id = g_device_id, .session = remote_session,
        .sequence = ++remote_sequence, .length = len + 1,
    };
    packet.payload[0] = type;
    memcpy(packet.payload + 1, data, len);
    gpio_set_level(LED_GPIO, 0);
    for (int attempt = 0; attempt < REMOTE_ATTEMPTS; attempt++) {
        if (!send_now(gateway_mac, &packet)) continue;
        int64_t deadline = esp_timer_get_time() + REMOTE_ACK_WAIT_MS * 1000;
        while (esp_timer_get_time() < deadline) {
            received_t rx;
            if (xQueueReceive(now_rx, &rx, pdMS_TO_TICKS(2)) != pdTRUE) continue;
            if (memcmp(rx.mac, gateway_mac, 6) || !authentic(&rx.packet) ||
                rx.packet.device_id != g_device_id) continue;
            remote_rssi = rx.rssi;
            if (rx.packet.kind == REMOTE_COMMAND) handle_remote_command(&rx.packet);
            if (rx.packet.kind == REMOTE_ACK && rx.packet.session == packet.session &&
                rx.packet.sequence == packet.sequence) {
                g_stats.mesh_send_success++;
                gpio_set_level(LED_GPIO, 1);
                return true;
            }
        }
    }
    gpio_set_level(LED_GPIO, 1);
    g_stats.mesh_send_failed++;
    ESP_LOGW(TAG, "No root acknowledgment for remote event %lu", (unsigned long)packet.sequence);
    return false;
}

typedef struct { int index, value; int64_t at; } button_edge_t;
static QueueHandle_t button_edges;
static int stable[NUM_BUTTONS], candidate[NUM_BUTTONS];
static int64_t changed[NUM_BUTTONS], pressed[NUM_BUTTONS], released[NUM_BUTTONS];
static bool ota_armed[NUM_BUTTONS];

static void IRAM_ATTR remote_button_isr(void* arg) {
    int i = (int)(intptr_t)arg;
    button_edge_t edge = {i, gpio_get_level(g_button_pins[i]), esp_timer_get_time()};
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(button_edges, &edge, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void commit_button(int i) {
    int value = candidate[i];
    stable[i] = value;
    if (value) {
        ota_armed[i] = released[i] &&
            changed[i] - released[i] < BUTTON_PRESS_OTA_INTERVAL_MS * 1000;
        pressed[i] = changed[i];
    } else {
        released[i] = changed[i];
        if (ota_armed[i] && changed[i] - pressed[i] >
            BUTTON_PRESS_OTA_THRESHOLD_MS * 1000) g_ota_requested = true;
    }
    g_stats.button_presses++;
    char payload[3] = {'a' + i, value ? '1' : '0',
        changed[i] - pressed[i] >= LONG_PRESS_THRESHOLD_MS * 1000 ? '1' : '0'};
    remote_send(MSG_TYPE_BUTTON, payload, value ? 2 : 3);
}

static void observe_button(int i, int value, int64_t at) {
    if (value == candidate[i]) return;
    // A complete debounced press can occur while a previous radio send is
    // waiting for its ACK. Commit it before consuming the next queued edge.
    if (candidate[i] != stable[i] && at - changed[i] >= BUTTON_DEBOUNCE_MS * 1000)
        commit_button(i);
    candidate[i] = value;
    changed[i] = at;
}

static void remote_status(void) {
    char json[REMOTE_PAYLOAD_SIZE - 1];
    uint64_t parent = 0;
    // Gateway device IDs use the softAP MAC, which is distinct from its STA
    // ESP-NOW address. Use provisioned gateway_id in telemetry separately if
    // available; parentId=0 explicitly denotes an unknown mesh parent.
    int len = snprintf(json, sizeof(json),
        "{\"deviceId\":%llu,\"parentId\":%llu,\"type\":\"remote\",\"firmware\":%llu,"
        "\"rssi\":%d,\"uptime\":%llu,\"clicks\":%lu,\"freeHeap\":%lu}",
        (unsigned long long)g_device_id, (unsigned long long)parent,
        (unsigned long long)g_firmware_timestamp, remote_rssi,
        (unsigned long long)(esp_timer_get_time()/1000000),
        (unsigned long)g_stats.button_presses, (unsigned long)esp_get_free_heap_size());
    if (len > 0 && len < sizeof(json)) remote_send(MSG_TYPE_STATUS, json, len);
}

void remote_switch_run(void) {
    unsigned int mac[6];
    int consumed = 0;
    if (!read_key() || sscanf(credentials_get()->gateway_mac,
        "%2x:%2x:%2x:%2x:%2x:%2x%n", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5],
        &consumed) != 6 || consumed != 17 || (mac[0] & 1)) {
        ESP_LOGE(TAG, "Remote requires valid gateway_mac and remote_key provisioning");
        return;
    }
    for (int i = 0; i < 6; i++) gateway_mac[i] = mac[i];
    do { remote_session = esp_random(); } while (!remote_session);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    radio_running = true;
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(credentials_get()->remote_channel, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(start_now());
    ota_check_rollback_on_boot();
    // SuperMini GPIO8 is an active-low discrete LED, not a WS2812.
    ESP_ERROR_CHECK(gpio_hold_dis(LED_GPIO));
    gpio_config_t led = {.pin_bit_mask = 1ULL << LED_GPIO, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&led));
    gpio_set_level(LED_GPIO, 1);
    // IDF's GPIO sleep workaround isolates pads by default. Keep the
    // SuperMini's active-low LED driven high instead of floating in sleep.
    ESP_ERROR_CHECK(gpio_sleep_sel_dis(LED_GPIO));
    button_edges = xQueueCreate(64, sizeof(button_edge_t));
    configASSERT(button_edges);
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_config_t io = {.pin_bit_mask = 1ULL << g_button_pins[i],
            .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .pull_up_en = GPIO_PULLUP_DISABLE, .intr_type = GPIO_INTR_ANYEDGE};
        ESP_ERROR_CHECK(gpio_config(&io));
        ESP_ERROR_CHECK(gpio_isr_handler_add(g_button_pins[i], remote_button_isr,
                                             (void*)(intptr_t)i));
    }
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
    int64_t next_status = 0;
    while (true) {
        button_edge_t edge;
        while (xQueueReceive(button_edges, &edge, 0) == pdTRUE)
            observe_button(edge.index, edge.value, edge.at);
        int64_t now = esp_timer_get_time();
        bool bouncing = false;
        for (int i = 0; i < NUM_BUTTONS; i++) {
            observe_button(i, gpio_get_level(g_button_pins[i]), now);
            if (candidate[i] != stable[i]) {
                if (now - changed[i] < BUTTON_DEBOUNCE_MS * 1000) bouncing = true;
                else commit_button(i);
            }
        }
        if (!bouncing && now >= next_status) {
            remote_status();
            next_status = now + REMOTE_HEARTBEAT_US;
        }
        // Process queued ping exchanges without recursive radio transactions.
        for (int round = 0; ping_pending && round < PING_PONG_NUMBER + 1; round++) {
            ping_pending = false;
            remote_send(MSG_TYPE_PING, &pending_ping, sizeof(pending_ping));
        }
        if (g_ota_requested) {
            g_ota_in_progress = true;
            gpio_set_level(LED_GPIO, 0);
            esp_now_deinit(); now_started = false;
            mesh_disconnect_and_ota();
            esp_restart(); // Failed/unconfigured OTA must not leave radio awake.
        }
        if (uxQueueMessagesWaiting(button_edges)) continue;
        if (radio_running) {
            // Briefly drain downlink (OTA/ping) arriving beside the root ACK.
            received_t rx;
            int64_t drain_end = esp_timer_get_time() + 10000;
            while (esp_timer_get_time() < drain_end &&
                   xQueueReceive(now_rx, &rx, pdMS_TO_TICKS(3)) == pdTRUE) {
                if (!memcmp(rx.mac, gateway_mac, 6) && authentic(&rx.packet) &&
                    rx.packet.device_id == g_device_id) handle_remote_command(&rx.packet);
            }
            if (g_ota_requested || ping_pending) continue;
            ESP_ERROR_CHECK(esp_wifi_stop());
            radio_running = false;
        }
        for (int i = 0; i < NUM_BUTTONS; i++) {
            ESP_ERROR_CHECK(gpio_intr_disable(g_button_pins[i]));
            ESP_ERROR_CHECK(gpio_wakeup_enable(g_button_pins[i], candidate[i] ?
                                               GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL));
        }
        int64_t sleep_now = esp_timer_get_time();
        int64_t remaining = next_status - sleep_now;
        if (bouncing && remaining <= 0) remaining = REMOTE_HEARTBEAT_US;
        for (int i = 0; i < NUM_BUTTONS; i++) {
            if (candidate[i] == stable[i]) continue;
            int64_t debounce_remaining = changed[i] + BUTTON_DEBOUNCE_MS * 1000 - sleep_now;
            if (debounce_remaining < remaining) remaining = debounce_remaining;
        }
        // Debounce waits sleep too; a new edge wakes immediately and restarts
        // that pin's timer. No periodic 2 ms CPU polling in remote mode.
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(remaining > 0 ? remaining : 1));
        esp_err_t err = ESP_OK;
        if (!uxQueueMessagesWaiting(button_edges)) {
            // Latch the physical pad high, not just its awake GPIO register.
            // Release after waking so transmission/OTA indications still work.
            ESP_ERROR_CHECK(gpio_set_level(LED_GPIO, 1));
            ESP_ERROR_CHECK(gpio_hold_en(LED_GPIO));
            err = esp_light_sleep_start();
            ESP_ERROR_CHECK(gpio_hold_dis(LED_GPIO));
        }
        for (int i = 0; i < NUM_BUTTONS; i++) {
            ESP_ERROR_CHECK(gpio_wakeup_disable(g_button_pins[i]));
            ESP_ERROR_CHECK(gpio_set_intr_type(g_button_pins[i], GPIO_INTR_ANYEDGE));
            ESP_ERROR_CHECK(gpio_intr_enable(g_button_pins[i]));
        }
        if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(2));
    }
}
#else
void remote_switch_run(void) { ESP_LOGE(TAG, "Remote switches require ESP32-C3"); }
#endif
