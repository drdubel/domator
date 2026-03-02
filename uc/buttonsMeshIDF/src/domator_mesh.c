/**
 * @file domator_mesh.c
 * @brief Application entry point and shared global state for the Domator
 *        ESP-MESH firmware.
 *
 * Responsibilities:
 *  - Defines all global variables declared as extern in domator_mesh.h.
 *  - Generates the unique device ID from the SoftAP MAC address.
 *  - Computes a firmware fingerprint from the ELF SHA-256 digest.
 *  - Auto-detects the hardware board type (switch / 8-relay / 16-relay).
 *  - Initialises FreeRTOS synchronisation primitives and launches all tasks
 *    from app_main().
 */

#include "domator_mesh.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "DOMATOR";

// ====================
// Global Variables
// ====================

uint64_t g_device_id = 0;
node_type_t g_node_type = NODE_TYPE_UNKNOWN;
device_stats_t g_stats = {0};
uint64_t g_firmware_timestamp = 0;

volatile bool g_mesh_connected = false;
volatile bool g_mesh_started = false;
volatile bool g_is_root = false;
volatile int g_mesh_layer = 0;
uint64_t g_parent_id = 0;

esp_mqtt_client_handle_t g_mqtt_client = NULL;
volatile bool g_mqtt_connected = false;

// Routing configuration (root only)
device_connections_t g_connections[MAX_NODES] = {0};
uint8_t g_num_devices = 0;
button_types_t g_button_types[MAX_NODES] = {0};
SemaphoreHandle_t g_connections_mutex = NULL;
SemaphoreHandle_t g_button_types_mutex = NULL;

button_state_t g_button_states[NUM_BUTTONS] = {0};
const int g_button_pins[NUM_BUTTONS] = {
    BUTTON_GPIO_0, BUTTON_GPIO_1, BUTTON_GPIO_2, BUTTON_GPIO_3,
    BUTTON_GPIO_4, BUTTON_GPIO_5, BUTTON_GPIO_6};
uint32_t g_last_root_contact = 0;

// Relay node globals
board_type_t g_board_type = BOARD_TYPE_8_RELAY;
uint16_t g_relay_outputs = 0;
button_state_t g_relay_button_states[NUM_RELAY_BUTTONS] = {0};
const int g_relay_8_pins[MAX_RELAYS_8] = {
    RELAY_8_PIN_0, RELAY_8_PIN_1, RELAY_8_PIN_2, RELAY_8_PIN_3,
    RELAY_8_PIN_4, RELAY_8_PIN_5, RELAY_8_PIN_6, RELAY_8_PIN_7};
const int g_relay_button_pins[NUM_RELAY_BUTTONS] = {
    RELAY_8_BUTTON_0, RELAY_8_BUTTON_1, RELAY_8_BUTTON_2, RELAY_8_BUTTON_3,
    RELAY_8_BUTTON_4, RELAY_8_BUTTON_5, RELAY_8_BUTTON_6, RELAY_8_BUTTON_7};
SemaphoreHandle_t g_relay_mutex = NULL;
peer_health_t g_peer_health[MAX_NODES] = {0};
uint8_t g_peer_count = 0;

QueueHandle_t g_mesh_tx_queue = NULL;
SemaphoreHandle_t g_stats_mutex = NULL;

TaskHandle_t button_task_handle = NULL;
TaskHandle_t telnet_task_handle = NULL;

volatile bool g_ota_in_progress = false;
volatile bool g_ota_requested = false;

mesh_addr_t g_broadcast_addr = {.addr = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

/**
 * @brief Derive a 48-bit device ID from the SoftAP MAC address and store it
 *        in g_device_id.  The ID is used as a stable, unique node identifier
 *        throughout the mesh and in MQTT topics.
 */
void generate_device_id(void) {
    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC address: %s", esp_err_to_name(ret));
        return;
    }

    g_device_id = (uint64_t)mac[0] << 40 | (uint64_t)mac[1] << 32 |
                  (uint64_t)mac[2] << 24 | (uint64_t)mac[3] << 16 |
                  (uint64_t)mac[4] << 8 | mac[5];

    ESP_LOGI(TAG, "Device ID: %" PRIu64 " (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
             g_device_id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/**
 * @brief Compute a firmware fingerprint from the SHA-256 digest of the running
 *        ELF binary.  The fingerprint is used in MQTT status messages to
 *        identify the exact firmware version running on each node.
 */
void build_time_to_unix(const char* build_time) {
    struct tm t = {0};
    char month_str[4];
    int day, year;
    int hour, min, sec;

    // Parse __DATE__ and __TIME__ format
    sscanf(build_time, "%3s %d %d %d:%d:%d", month_str, &day, &year, &hour,
           &min, &sec);

    // Convert month string to number
    const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int mon = 0;
    for (int i = 0; i < 12; i++) {
        if (strncmp(month_str, months[i], 3) == 0) {
            mon = i;
            break;
        }
    }

    t.tm_year = year - 1900;
    t.tm_mon = mon;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;

    g_firmware_timestamp = (uint64_t)mktime(&t);
    ESP_LOGI(TAG, "Firmware build time: %s -> timestamp: %" PRIu64, build_time,
             g_firmware_timestamp);
}

/**
 * @brief Determine the hardware board type and set g_node_type/g_board_type.
 *
 * Detection priority:
 *  1. NVS key ``hardware_type`` (0=switch, 1=relay_8, 2=relay_16) overrides
 *     everything.
 *  2. On ESP32-C3 targets the hardware is always SWITCH_C3 (relay boards
 *     require the classic ESP32 GPIO range).
 *  3. Probe shift-register pins (GPIO 12-14): all high with pull-up means a
 *     16-relay board is attached.
 *  4. Probe relay-specific GPIOs (32, 33, 25): accessible on ESP32 means an
 *     8-relay board is attached.
 *  5. Default to SWITCH_C3.
 */
void detect_hardware_type(void) {
    ESP_LOGI(TAG, "Starting hardware detection...");
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("domator", NVS_READONLY, &nvs_handle);
    if (ret == ESP_OK) {
        uint8_t hw_type = 0;
        ret = nvs_get_u8(nvs_handle, "hardware_type", &hw_type);
        if (ret == ESP_OK) {
            if (hw_type == 1) {
                g_node_type = NODE_TYPE_RELAY_8;
                g_board_type = BOARD_TYPE_8_RELAY;
                ESP_LOGI(TAG, "Hardware type from NVS: RELAY_8 (override)");
                nvs_close(nvs_handle);
                return;
            } else if (hw_type == 2) {
                g_node_type = NODE_TYPE_RELAY_16;
                g_board_type = BOARD_TYPE_16_RELAY;
                ESP_LOGI(TAG, "Hardware type from NVS: RELAY_16 (override)");
                nvs_close(nvs_handle);
                return;
            } else if (hw_type == 0) {
                g_node_type = NODE_TYPE_SWITCH_C3;
                ESP_LOGI(TAG, "Hardware type from NVS: SWITCH_C3 (override)");
                nvs_close(nvs_handle);
                return;
            }
        }
        nvs_close(nvs_handle);
    }

#ifdef CONFIG_IDF_TARGET_ESP32C3
    ESP_LOGI(TAG, "ESP32-C3 detected - skipping hardware auto-detection");
    ESP_LOGI(TAG, "Defaulting to SWITCH_C3 mode (ESP32-C3 primary use case)");
    g_node_type = NODE_TYPE_SWITCH_C3;
    return;
#endif

    ESP_LOGD(TAG, "Checking for 16-relay board (shift register pins)...");

    gpio_config_t io_conf = {.pin_bit_mask = ((1ULL << RELAY_16_PIN_DATA) |
                                              (1ULL << RELAY_16_PIN_CLOCK) |
                                              (1ULL << RELAY_16_PIN_LATCH)),
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .intr_type = GPIO_INTR_DISABLE};

    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure shift register pins: %s",
                 esp_err_to_name(ret));
        g_node_type = NODE_TYPE_SWITCH_C3;
        ESP_LOGI(
            TAG,
            "Hardware detected as: SWITCH_C3 (default due to config error)");
        return;
    }

    ESP_LOGD(TAG, "Shift register pins configured, waiting for settle...");
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGD(TAG, "Reading shift register pin states...");
    int data_val = gpio_get_level(RELAY_16_PIN_DATA);
    int clock_val = gpio_get_level(RELAY_16_PIN_CLOCK);
    int latch_val = gpio_get_level(RELAY_16_PIN_LATCH);

    ESP_LOGD(TAG, "Pin states: DATA=%d, CLOCK=%d, LATCH=%d", data_val,
             clock_val, latch_val);

    if (data_val && clock_val && latch_val) {
        g_node_type = NODE_TYPE_RELAY_16;
        g_board_type = BOARD_TYPE_16_RELAY;
        ESP_LOGI(TAG, "Hardware detected as: RELAY_16");
        return;
    }

    ESP_LOGD(TAG, "16-relay board not detected, checking for 8-relay...");

    ESP_LOGD(TAG, "Probing 8-relay specific GPIOs (32, 33, 25)...");

    io_conf.pin_bit_mask = ((1ULL << 32) | (1ULL << 33) | (1ULL << 25));
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;

    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        // If these GPIOs can't be configured, not an 8-relay board
        ESP_LOGD(TAG, "Could not configure 8-relay GPIOs: %s",
                 esp_err_to_name(ret));
        g_node_type = NODE_TYPE_SWITCH_C3;
        ESP_LOGI(
            TAG,
            "Hardware detected as: SWITCH_C3 (8-relay GPIOs not available)");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    int gpio32_val = gpio_get_level(32);
    int gpio33_val = gpio_get_level(33);
    int gpio25_val = gpio_get_level(25);

    ESP_LOGD(TAG, "8-relay GPIO states: 32=%d, 33=%d, 25=%d", gpio32_val,
             gpio33_val, gpio25_val);

    io_conf.pin_bit_mask = (1ULL << 0);
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(5));
        int gpio0_val = gpio_get_level(0);
        ESP_LOGD(TAG, "Switch GPIO 0 state: %d", gpio0_val);
    }

#ifdef CONFIG_IDF_TARGET_ESP32
    g_node_type = NODE_TYPE_RELAY_8;
    g_board_type = BOARD_TYPE_8_RELAY;
    ESP_LOGI(
        TAG,
        "Hardware detected as: RELAY_8 (ESP32 with relay GPIOs accessible)");
    ESP_LOGI(TAG,
             "If this is incorrect, set hardware_type in NVS (0=switch, "
             "1=relay_8, 2=relay_16)");
#else
    g_node_type = NODE_TYPE_SWITCH_C3;
    ESP_LOGI(TAG, "Hardware detected as: SWITCH_C3 (default for non-ESP32)");
    ESP_LOGW(TAG,
             "Set hardware_type in NVS if this is incorrect (0=switch, "
             "1=relay_8, 2=relay_16)");
#endif
}

/**
 * @brief Firmware entry point.
 *
 * Execution order:
 *  1. Initialise NVS flash.
 *  2. Generate device ID and firmware timestamp.
 *  3. Detect hardware type.
 *  4. Create all FreeRTOS queues and mutexes.
 *  5. Pre-initialise relay hardware if this is a relay node.
 *  6. Start the mesh network stack.
 *  7. Launch shared tasks: mesh RX/TX, status reporter, health monitor, OTA.
 *  8. Launch node-type-specific tasks (button/LED for switch, relay buttons
 *     for relay boards).
 */
void app_main(void) {
    ESP_LOGI(TAG, "Domator Mesh starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    generate_device_id();
    build_time_to_unix(FW_BUILD_TIME);
    detect_hardware_type();

    g_mesh_tx_queue = xQueueCreate(MESH_TX_QUEUE_SIZE, sizeof(mesh_app_msg_t));
    if (g_mesh_tx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create mesh TX queue");
        return;
    }

    g_stats_mutex = xSemaphoreCreateMutex();
    if (g_stats_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create stats mutex");
        return;
    }

    g_connections_mutex = xSemaphoreCreateMutex();
    if (g_connections_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create connections mutex");
        return;
    }

    g_button_types_mutex = xSemaphoreCreateMutex();
    if (g_button_types_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create button types mutex");
        return;
    }

    if (g_node_type == NODE_TYPE_RELAY_8 || g_node_type == NODE_TYPE_RELAY_16) {
        g_relay_mutex = xSemaphoreCreateMutex();
        if (g_relay_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create relay mutex");
            return;
        }

        ESP_LOGI(TAG, "Pre-initializing relay board (type: %s)",
                 g_board_type == BOARD_TYPE_16_RELAY ? "16-relay" : "8-relay");
        relay_init();
        relay_button_init();
    }

    mesh_network_init();

    xTaskCreate(mesh_rx_task, "mesh_rx", 8192, NULL, 5, NULL);
    xTaskCreate(mesh_tx_task, "mesh_tx", 4096, NULL, 4, NULL);
    xTaskCreate(status_report_task, "status", 4096, NULL, 1, NULL);
    xTaskCreate(health_monitor_task, "health_monitor", 3072, NULL, 2, NULL);
    xTaskCreate(ota_task, "ota", 8192, NULL, 10, NULL);

    // Start node-specific tasks
    if (g_node_type == NODE_TYPE_SWITCH_C3) {
        ESP_LOGI(TAG, "Starting switch node tasks");
        button_init();
        led_init();
        xTaskCreate(button_task, "button", 4096, NULL, 6, &button_task_handle);
        xTaskCreate(led_task, "led", 3072, NULL, 2, NULL);
    } else if (g_node_type == NODE_TYPE_RELAY_8 ||
               g_node_type == NODE_TYPE_RELAY_16) {
        ESP_LOGI(TAG, "Starting relay tasks (hardware already initialized)");
        xTaskCreate(relay_button_task, "relay_button", 5120, NULL, 6,
                    &button_task_handle);
    }

    ESP_LOGI(TAG, "Domator Mesh initialized");
}