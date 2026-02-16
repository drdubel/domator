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

// ==================== OTA Functions ====================
void ota_start_update(const char* url) {
    if (url == NULL || strlen(url) == 0) {
        ESP_LOGW(TAG, "Invalid OTA URL");
        return;
    }

    ESP_LOGI(TAG, "Starting OTA update from: %s", url);
    g_ota_in_progress = true;

    mesh_stop_and_connect_sta();
    if (g_board_type == BOARD_TYPE_8_RELAY ||
        g_board_type == BOARD_TYPE_16_RELAY) {
        relay_save_states_to_nvs();
    }

    // Configure HTTP client for OTA
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    ESP_LOGI(TAG, "Starting HTTPS OTA update...");
    esp_err_t ret = esp_https_ota(&ota_config);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA update successful, restarting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(ret));
        g_ota_in_progress = false;
    }
}

void ota_task(void* arg) {
    uint32_t ota_countdown = 0;
    bool ota_countdown_active = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (g_ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (g_ota_requested) {
            g_ota_requested = false;
            ota_countdown_active = true;
        }

        if (!ota_countdown_active) {
            ota_countdown = esp_timer_get_time() / 1000;  // ms
            continue;
        } else if ((esp_timer_get_time() / 1000) - ota_countdown >=
                   OTA_COUNTDOWN_MS) {
            ota_countdown_active = false;
            ota_start_update(CONFIG_OTA_URL);
        }
    }
}

// ==================== Health Monitoring ====================
void health_monitor_task(void* arg) {
    ESP_LOGI(TAG, "Health monitor task started");

    uint32_t last_low_heap_log = 0;
    uint32_t last_critical_heap_log = 0;

    while (1) {
        if (g_ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 seconds
            continue;
        }

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