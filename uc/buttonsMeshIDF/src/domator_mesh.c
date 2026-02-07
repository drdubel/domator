#include "domator_mesh.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "DOMATOR_MESH";

// ====================
// Global Variables
// ====================

uint32_t g_device_id = 0;
node_type_t g_node_type = NODE_TYPE_UNKNOWN;
char g_firmware_hash[33] = {0};
device_stats_t g_stats = {0};

bool g_mesh_connected = false;
bool g_mesh_started = false;
bool g_is_root = false;
int g_mesh_layer = 0;
uint32_t g_parent_id = 0;

esp_mqtt_client_handle_t g_mqtt_client = NULL;
bool g_mqtt_connected = false;

// Routing configuration (root only)
device_connections_t g_connections[MAX_DEVICES] = {0};
uint32_t g_device_ids[MAX_DEVICES] = {0};
uint8_t g_num_devices = 0;
uint8_t g_button_types[MAX_DEVICES][16] = {0};
SemaphoreHandle_t g_connections_mutex = NULL;
SemaphoreHandle_t g_button_types_mutex = NULL;

button_state_t g_button_states[NUM_BUTTONS] = {0};
const int g_button_pins[NUM_BUTTONS] = {
    BUTTON_GPIO_0, BUTTON_GPIO_1, BUTTON_GPIO_2, BUTTON_GPIO_3,
    BUTTON_GPIO_4, BUTTON_GPIO_5, BUTTON_GPIO_6
};
button_gesture_config_t g_gesture_config[NUM_BUTTONS] = {0};
uint32_t g_last_root_contact = 0;

// Relay node globals
board_type_t g_board_type = BOARD_TYPE_8_RELAY;
uint16_t g_relay_outputs = 0;
button_state_t g_relay_button_states[NUM_RELAY_BUTTONS] = {0};
const int g_relay_8_pins[MAX_RELAYS_8] = {
    RELAY_8_PIN_0, RELAY_8_PIN_1, RELAY_8_PIN_2, RELAY_8_PIN_3,
    RELAY_8_PIN_4, RELAY_8_PIN_5, RELAY_8_PIN_6, RELAY_8_PIN_7
};
const int g_relay_button_pins[NUM_RELAY_BUTTONS] = {
    RELAY_8_BUTTON_0, RELAY_8_BUTTON_1, RELAY_8_BUTTON_2, RELAY_8_BUTTON_3,
    RELAY_8_BUTTON_4, RELAY_8_BUTTON_5, RELAY_8_BUTTON_6, RELAY_8_BUTTON_7
};
SemaphoreHandle_t g_relay_mutex = NULL;
peer_health_t g_peer_health[MAX_DEVICES] = {0};
uint8_t g_peer_count = 0;

QueueHandle_t g_mesh_tx_queue = NULL;
SemaphoreHandle_t g_stats_mutex = NULL;

bool g_ota_in_progress = false;

// ====================
// Device ID Generation
// ====================

void generate_device_id(void)
{
    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC address: %s", esp_err_to_name(ret));
        return;
    }
    
    // Generate device ID from last 4 bytes of MAC address
    g_device_id = (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5];
    
    ESP_LOGI(TAG, "Device ID: %" PRIu32 " (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
             g_device_id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ====================
// Firmware Hash Generation
// ====================

void generate_firmware_hash(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    
    if (app_desc != NULL) {
        // Use the SHA256 hash from the ELF file for a true firmware hash
        // Convert first 16 bytes to hex string (32 chars)
        for (int i = 0; i < 16; i++) {
            sprintf(&g_firmware_hash[i * 2], "%02x", app_desc->app_elf_sha256[i]);
        }
        ESP_LOGI(TAG, "Firmware version: %s, hash: %s", 
                 app_desc->version, g_firmware_hash);
    } else {
        snprintf(g_firmware_hash, sizeof(g_firmware_hash), "unknown");
        ESP_LOGW(TAG, "Could not read app description");
    }
}

// ====================
// Hardware Type Detection
// ====================

void detect_hardware_type(void)
{
    // Hardware auto-detection strategy:
    // 1. Check NVS for manual configuration override
    // 2. Check for 16-relay board by probing shift register pins (14, 13, 12)
    // 3. Check for 8-relay board by probing relay-specific GPIOs (32, 33, 25)
    // 4. Default to switch node if neither relay board is detected
    
    ESP_LOGI(TAG, "Starting hardware detection...");
    
    // Check NVS for hardware type override
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("domator", NVS_READONLY, &nvs_handle);
    if (ret == ESP_OK) {
        uint8_t hw_type = 0;
        ret = nvs_get_u8(nvs_handle, "hardware_type", &hw_type);
        if (ret == ESP_OK) {
            if (hw_type == 1) {
                g_node_type = NODE_TYPE_RELAY;
                g_board_type = BOARD_TYPE_8_RELAY;
                ESP_LOGI(TAG, "Hardware type from NVS: RELAY_8 (override)");
                nvs_close(nvs_handle);
                return;
            } else if (hw_type == 2) {
                g_node_type = NODE_TYPE_RELAY;
                g_board_type = BOARD_TYPE_16_RELAY;
                ESP_LOGI(TAG, "Hardware type from NVS: RELAY_16 (override)");
                nvs_close(nvs_handle);
                return;
            } else if (hw_type == 0) {
                g_node_type = NODE_TYPE_SWITCH;
                ESP_LOGI(TAG, "Hardware type from NVS: SWITCH (override)");
                nvs_close(nvs_handle);
                return;
            }
        }
        nvs_close(nvs_handle);
    }
    
    // On ESP32-C3, skip auto-detection and default to switch mode
    // This avoids potential issues with GPIO probing on ESP32-C3
    // ESP32-C3 is the primary target for switch nodes (per README)
    // Relay boards are designed for ESP32 (original), not ESP32-C3
#ifdef CONFIG_IDF_TARGET_ESP32C3
    ESP_LOGI(TAG, "ESP32-C3 detected - skipping hardware auto-detection");
    ESP_LOGI(TAG, "Defaulting to SWITCH mode (ESP32-C3 primary use case)");
    ESP_LOGI(TAG, "To use relay board on ESP32-C3, configure node type via NVS");
    g_node_type = NODE_TYPE_SWITCH;
    return;
#endif
    
    // First, check for 16-relay board (shift register)
    ESP_LOGD(TAG, "Checking for 16-relay board (shift register pins)...");
    
    gpio_config_t io_conf = {
        .pin_bit_mask = ((1ULL << RELAY_16_PIN_DATA) | 
                         (1ULL << RELAY_16_PIN_CLOCK) | 
                         (1ULL << RELAY_16_PIN_LATCH)),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure shift register pins: %s", esp_err_to_name(ret));
        g_node_type = NODE_TYPE_SWITCH;
        ESP_LOGI(TAG, "Hardware detected as: SWITCH (default due to config error)");
        return;
    }
    
    ESP_LOGD(TAG, "Shift register pins configured, waiting for settle...");
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Read the pins - if all high with pullup, likely shift register present
    ESP_LOGD(TAG, "Reading shift register pin states...");
    int data_val = gpio_get_level(RELAY_16_PIN_DATA);
    int clock_val = gpio_get_level(RELAY_16_PIN_CLOCK);
    int latch_val = gpio_get_level(RELAY_16_PIN_LATCH);
    
    ESP_LOGD(TAG, "Pin states: DATA=%d, CLOCK=%d, LATCH=%d", data_val, clock_val, latch_val);
    
    if (data_val && clock_val && latch_val) {
        g_node_type = NODE_TYPE_RELAY;
        g_board_type = BOARD_TYPE_16_RELAY;
        ESP_LOGI(TAG, "Hardware detected as: RELAY_16");
        return;
    }
    
    ESP_LOGD(TAG, "16-relay board not detected, checking for 8-relay...");
    
    // Check for 8-relay board by probing relay-specific GPIOs
    // 8-relay boards use GPIOs: 32, 33, 25, 26, 27, 14, 12, 13
    // Switch boards use GPIOs: 0-6 for buttons (completely different)
    // Key discriminator: GPIO 32, 33, 25 exist and are usable on 8-relay but not typically on switch
    
    ESP_LOGD(TAG, "Probing 8-relay specific GPIOs (32, 33, 25)...");
    
    // Configure GPIO 32, 33, 25 as inputs with pull-down to test accessibility
    io_conf.pin_bit_mask = ((1ULL << 32) | (1ULL << 33) | (1ULL << 25));
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    
    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        // If these GPIOs can't be configured, not an 8-relay board
        ESP_LOGD(TAG, "Could not configure 8-relay GPIOs: %s", esp_err_to_name(ret));
        g_node_type = NODE_TYPE_SWITCH;
        ESP_LOGI(TAG, "Hardware detected as: SWITCH (8-relay GPIOs not available)");
        return;
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Read the GPIO levels
    int gpio32_val = gpio_get_level(32);
    int gpio33_val = gpio_get_level(33);
    int gpio25_val = gpio_get_level(25);
    
    ESP_LOGD(TAG, "8-relay GPIO states: 32=%d, 33=%d, 25=%d", gpio32_val, gpio33_val, gpio25_val);
    
    // Now check for switch-specific GPIO 0 (first button on switch)
    // If GPIO 0 is configured as button input, this is likely a switch
    io_conf.pin_bit_mask = (1ULL << 0);
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(5));
        int gpio0_val = gpio_get_level(0);
        ESP_LOGD(TAG, "Switch GPIO 0 state: %d", gpio0_val);
        
        // If GPIO 0 is accessible and the relay GPIOs aren't being used,
        // this is likely a switch board
        // However, this is still not 100% reliable
    }
    
    // Decision logic:
    // If we successfully probed 8-relay specific GPIOs (32, 33, 25), 
    // and this is an ESP32 (not ESP32-C3), assume it's an 8-relay board
    // ESP32 has these GPIOs available, switch boards typically don't use them
    
#ifdef CONFIG_IDF_TARGET_ESP32
    // On ESP32, if we can access GPIO 32/33/25, assume 8-relay board
    // This is the most common configuration
    g_node_type = NODE_TYPE_RELAY;
    g_board_type = BOARD_TYPE_8_RELAY;
    ESP_LOGI(TAG, "Hardware detected as: RELAY_8 (ESP32 with relay GPIOs accessible)");
    ESP_LOGI(TAG, "If this is incorrect, set hardware_type in NVS (0=switch, 1=relay_8, 2=relay_16)");
#else
    // Other ESP32 variants - default to switch
    g_node_type = NODE_TYPE_SWITCH;
    ESP_LOGI(TAG, "Hardware detected as: SWITCH (default for non-ESP32)");
    ESP_LOGW(TAG, "Set hardware_type in NVS if this is incorrect (0=switch, 1=relay_8, 2=relay_16)");
#endif
}

// ====================
// Main Entry Point
// ====================

void app_main(void)
{
    ESP_LOGI(TAG, "Domator Mesh starting...");
    ESP_LOGI(TAG, "IDF Version: %s", esp_get_idf_version());
    
    // Initialize device identity
    generate_device_id();
    generate_firmware_hash();
    detect_hardware_type();
    
    // Create synchronization primitives
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
    
    // Create routing mutexes for root node
    if (g_is_root || g_node_type == NODE_TYPE_ROOT) {
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
        
        root_init_routing();
    }
    
    // Create relay mutex if needed (BEFORE mesh_init to prevent race conditions)
    if (g_node_type == NODE_TYPE_RELAY) {
        g_relay_mutex = xSemaphoreCreateMutex();
        if (g_relay_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create relay mutex");
            return;
        }
        
        // Initialize relay hardware BEFORE mesh to prevent race conditions
        // This ensures relays are ready before mesh events can trigger relay operations
        ESP_LOGI(TAG, "Pre-initializing relay board (type: %s)",
                 g_board_type == BOARD_TYPE_16_RELAY ? "16-relay" : "8-relay");
        relay_init();
        relay_button_init();
    }
    
    // Initialize mesh network (after relay hardware is ready)
    mesh_init();
    
    // Wait for mesh to be ready
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Start communication tasks with increased stack for reliability
    xTaskCreate(mesh_send_task, "mesh_send", 5120, NULL, 5, NULL);
    xTaskCreate(mesh_recv_task, "mesh_recv", 5120, NULL, 5, NULL);
    xTaskCreate(status_report_task, "status_report", 4096, NULL, 3, NULL);
    
    // Start node-specific tasks
    if (g_node_type == NODE_TYPE_SWITCH) {
        ESP_LOGI(TAG, "Starting switch node tasks");
        button_init();
        led_init();
        xTaskCreate(button_task, "button", 4096, NULL, 6, NULL);
        xTaskCreate(led_task, "led", 3072, NULL, 2, NULL);
        xTaskCreate(root_loss_check_task, "root_loss", 3072, NULL, 2, NULL);
    } else if (g_node_type == NODE_TYPE_RELAY) {
        ESP_LOGI(TAG, "Starting relay tasks (hardware already initialized)");
        xTaskCreate(relay_button_task, "relay_button", 5120, NULL, 6, NULL);
    }
    
    // Start health monitoring task for all nodes
    xTaskCreate(health_monitor_task, "health_monitor", 3072, NULL, 2, NULL);
    
    // Start peer health check task for root
    if (g_is_root || g_node_type == NODE_TYPE_ROOT) {
        xTaskCreate(peer_health_check_task, "peer_health", 3072, NULL, 2, NULL);
    }
    
    ESP_LOGI(TAG, "Domator Mesh initialized");
}
