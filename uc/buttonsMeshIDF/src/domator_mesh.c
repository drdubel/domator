#include "domator_mesh.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "driver/gpio.h"

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

button_state_t g_button_states[NUM_BUTTONS] = {0};
const int g_button_pins[NUM_BUTTONS] = {
    BUTTON_GPIO_0, BUTTON_GPIO_1, BUTTON_GPIO_2, BUTTON_GPIO_3,
    BUTTON_GPIO_4, BUTTON_GPIO_5, BUTTON_GPIO_6
};

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
    // 1. Check for 16-relay board by probing shift register pins (14, 13, 12)
    // 2. Check for 8-relay board by probing relay output pins (32, 33, etc.)
    // 3. Default to switch node if neither relay board is detected
    //
    // LIMITATION: Currently cannot distinguish between 8-relay board and switch node
    // reliably because both use similar GPIO configurations. Consider using NVS
    // configuration or a dedicated detection pin for production deployments.
    
    // First, check for 16-relay board (shift register)
    gpio_config_t io_conf = {
        .pin_bit_mask = ((1ULL << RELAY_16_PIN_DATA) | 
                         (1ULL << RELAY_16_PIN_CLOCK) | 
                         (1ULL << RELAY_16_PIN_LATCH)),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    gpio_config(&io_conf);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Read the pins - if all high with pullup, likely shift register present
    int data_val = gpio_get_level(RELAY_16_PIN_DATA);
    int clock_val = gpio_get_level(RELAY_16_PIN_CLOCK);
    int latch_val = gpio_get_level(RELAY_16_PIN_LATCH);
    
    if (data_val && clock_val && latch_val) {
        g_node_type = NODE_TYPE_RELAY;
        g_board_type = BOARD_TYPE_16_RELAY;
        ESP_LOGI(TAG, "Hardware detected as: RELAY_16");
        return;
    }
    
    // Check for 8-relay board by probing the first relay output pin (GPIO 32)
    // This GPIO is less likely to be used on switch boards
    io_conf.pin_bit_mask = (1ULL << RELAY_8_PIN_0);
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
    vTaskDelay(pdMS_TO_TICKS(5));
    
    // Try to probe GPIO 32 - if it exists and can be configured, might be 8-relay board
    // However, this is not definitive. For production, use NVS configuration.
    
    // Default to switch for now - user should configure via NVS for 8-relay boards
    g_node_type = NODE_TYPE_SWITCH;
    ESP_LOGI(TAG, "Hardware detected as: SWITCH_C3");
    ESP_LOGW(TAG, "Cannot distinguish 8-relay from switch - configure via NVS if needed");
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
    
    // Create relay mutex if needed
    if (g_node_type == NODE_TYPE_RELAY) {
        g_relay_mutex = xSemaphoreCreateMutex();
        if (g_relay_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create relay mutex");
            return;
        }
    }
    
    // Initialize mesh network
    mesh_init();
    
    // Wait for mesh to be ready
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Start communication tasks
    xTaskCreate(mesh_send_task, "mesh_send", 4096, NULL, 5, NULL);
    xTaskCreate(mesh_recv_task, "mesh_recv", 4096, NULL, 5, NULL);
    xTaskCreate(status_report_task, "status_report", 4096, NULL, 3, NULL);
    
    // Start node-specific tasks
    if (g_node_type == NODE_TYPE_SWITCH) {
        ESP_LOGI(TAG, "Starting switch node tasks");
        button_init();
        led_init();
        xTaskCreate(button_task, "button", 4096, NULL, 6, NULL);
        xTaskCreate(led_task, "led", 3072, NULL, 2, NULL);
    } else if (g_node_type == NODE_TYPE_RELAY) {
        ESP_LOGI(TAG, "Starting relay node tasks (board type: %s)",
                 g_board_type == BOARD_TYPE_16_RELAY ? "16-relay" : "8-relay");
        relay_init();
        relay_button_init();
        xTaskCreate(relay_button_task, "relay_button", 4096, NULL, 6, NULL);
    }
    
    ESP_LOGI(TAG, "Domator Mesh initialized");
}
