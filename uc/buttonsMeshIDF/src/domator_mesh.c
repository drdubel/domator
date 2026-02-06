#include "domator_mesh.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"

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
        // Use the app version and SHA256 hash from partition
        snprintf(g_firmware_hash, sizeof(g_firmware_hash), "%.8s", app_desc->version);
        ESP_LOGI(TAG, "Firmware version: %s, project: %s", 
                 app_desc->version, app_desc->project_name);
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
    // For now, hardcode to SWITCH for Phase 2
    // In future, this could detect based on GPIO configuration or NVS setting
    g_node_type = NODE_TYPE_SWITCH;
    ESP_LOGI(TAG, "Hardware detected as: SWITCH_C3");
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
    }
    
    ESP_LOGI(TAG, "Domator Mesh initialized");
}
