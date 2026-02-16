#include <inttypes.h>
#include <string.h>

#include "domator_mesh.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

static const char* TAG = "HEALTH_OTA";

static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t* update_partition = NULL;

// ====================
// OTA Functions
// ====================

void handle_ota_message(mesh_addr_t* from, mesh_app_msg_t* msg) {
    switch (msg->msg_type) {
        case MSG_TYPE_OTA_START:
            g_ota_in_progress = true;

            update_partition = esp_ota_get_next_update_partition(NULL);
            ESP_ERROR_CHECK(
                esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle));

            ESP_LOGI(TAG, "OTA started");
            break;

        case MSG_TYPE_OTA_DATA:
            ESP_ERROR_CHECK(
                esp_ota_write(ota_handle, msg->data, msg->data_len));
            break;

        case MSG_TYPE_OTA_END:
            ESP_ERROR_CHECK(esp_ota_end(ota_handle));
            ESP_ERROR_CHECK(esp_ota_set_boot_partition(update_partition));

            ESP_LOGI(TAG, "OTA finished, rebooting...");
            esp_restart();
            break;
    }
}

// ====================
// Health Monitoring
// ====================

void health_monitor_task(void* arg) {
    ESP_LOGI(TAG, "Health monitor task started");

    uint32_t last_low_heap_log = 0;
    uint32_t last_critical_heap_log = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 seconds

        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t current_time = esp_timer_get_time() / 1000;  // ms

        // Check for low heap
        if (free_heap < LOW_HEAP_THRESHOLD) {
            // Log at most once per minute
            if (current_time - last_low_heap_log > 60000) {
                ESP_LOGW(TAG, "Low heap detected: %lu bytes free", free_heap);
                last_low_heap_log = current_time;

                if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) ==
                    pdTRUE) {
                    g_stats.low_heap_events++;
                    xSemaphoreGive(g_stats_mutex);
                }
            }
        }

        // Check for critical heap
        if (free_heap < CRITICAL_HEAP_THRESHOLD) {
            // Log at most once per minute
            if (current_time - last_critical_heap_log > 60000) {
                ESP_LOGE(TAG, "CRITICAL heap level: %lu bytes free", free_heap);
                last_critical_heap_log = current_time;

                if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) ==
                    pdTRUE) {
                    g_stats.critical_heap_events++;
                    xSemaphoreGive(g_stats_mutex);
                }
            }
        }
    }
}