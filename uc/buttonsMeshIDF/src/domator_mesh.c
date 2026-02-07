#include "domator_mesh.h"

static const char* TAG = "DOMATOR";

node_type_t g_node_type = NODE_TYPE_UNKNOWN;
uint32_t g_device_id = 0;
bool g_is_root = false;
bool g_mesh_connected = false;

void app_main(void) {
    ESP_LOGI(TAG, "Domator Mesh starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    g_node_type = NODE_TYPE_SWITCH_C3;

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    g_device_id = (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5];
    ESP_LOGI(TAG, "Device ID: %" PRIu32, g_device_id);

    mesh_network_init();

    xTaskCreate(mesh_rx_task, "mesh_rx", 8192, NULL, 5, NULL);
    xTaskCreate(mesh_tx_task, "mesh_tx", 4096, NULL, 4, NULL);
    xTaskCreate(status_report_task, "status", 4096, NULL, 1, NULL);
}