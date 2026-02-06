#include "domator_mesh.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "HEALTH_OTA";

// ====================
// OTA Functions
// ====================

void ota_init(void)
{
    ESP_LOGI(TAG, "OTA initialized");
    // OTA infrastructure is ready via esp_https_ota component
}

void ota_start_update(const char *url)
{
    if (url == NULL || strlen(url) == 0) {
        ESP_LOGW(TAG, "Invalid OTA URL");
        return;
    }
    
    ESP_LOGI(TAG, "Starting OTA update from: %s", url);
    g_ota_in_progress = true;
    
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

void ota_trigger_from_mesh(const char *url)
{
    if (url == NULL || strlen(url) == 0) {
        ESP_LOGW(TAG, "Invalid OTA URL from mesh");
        return;
    }
    
    ESP_LOGI(TAG, "OTA triggered via mesh: %s", url);
    
    // Start OTA in a separate task to avoid blocking mesh communication
    // We use a simple approach: create a task, pass the URL via global, and let it handle OTA
    // Note: In production, you'd want better error handling and state management
    
    // For now, we'll do a direct call since OTA will restart the device anyway
    ota_start_update(url);
}

// ====================
// Health Monitoring
// ====================

void health_monitor_task(void *arg)
{
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
                
                if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
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
                
                if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    g_stats.critical_heap_events++;
                    xSemaphoreGive(g_stats_mutex);
                }
            }
        }
    }
}

// ====================
// Peer Health Tracking (Root Node)
// ====================

void peer_health_update(uint32_t device_id, int8_t rssi)
{
    uint32_t current_time = esp_timer_get_time() / 1000;  // ms
    
    // Find peer in tracking table
    int peer_idx = -1;
    for (uint8_t i = 0; i < g_peer_count; i++) {
        if (g_peer_health[i].device_id == device_id) {
            peer_idx = i;
            break;
        }
    }
    
    // Add new peer if not found
    if (peer_idx < 0) {
        if (g_peer_count < MAX_DEVICES) {
            peer_idx = g_peer_count;
            g_peer_health[peer_idx].device_id = device_id;
            g_peer_health[peer_idx].disconnect_count = 0;
            g_peer_count++;
            ESP_LOGI(TAG, "Added peer %" PRIu32 " to health tracking", device_id);
        } else {
            ESP_LOGW(TAG, "Peer health table full, cannot add device %" PRIu32, device_id);
            return;
        }
    }
    
    // Update peer health
    g_peer_health[peer_idx].last_seen = current_time;
    g_peer_health[peer_idx].last_rssi = rssi;
    g_peer_health[peer_idx].is_alive = true;
}

void peer_health_check_task(void *arg)
{
    ESP_LOGI(TAG, "Peer health check task started");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(PEER_HEALTH_CHECK_INTERVAL_MS));
        
        uint32_t current_time = esp_timer_get_time() / 1000;  // ms
        uint32_t timeout_threshold = 60000;  // 60 seconds
        
        // Check each peer for timeout
        for (uint8_t i = 0; i < g_peer_count; i++) {
            if (g_peer_health[i].is_alive) {
                uint32_t time_since_seen = current_time - g_peer_health[i].last_seen;
                
                if (time_since_seen > timeout_threshold) {
                    g_peer_health[i].is_alive = false;
                    g_peer_health[i].disconnect_count++;
                    
                    ESP_LOGW(TAG, "Peer %" PRIu32 " timeout (last seen %lu ms ago, disconnect count: %lu)",
                             g_peer_health[i].device_id, time_since_seen, g_peer_health[i].disconnect_count);
                }
            }
        }
        
        // Log health summary
        uint8_t alive_count = 0;
        for (uint8_t i = 0; i < g_peer_count; i++) {
            if (g_peer_health[i].is_alive) {
                alive_count++;
            }
        }
        
        ESP_LOGI(TAG, "Peer health: %d/%d peers alive", alive_count, g_peer_count);
    }
}

// ====================
// Root Loss Detection (Leaf Nodes)
// ====================

void root_loss_check_task(void *arg)
{
    ESP_LOGI(TAG, "Root loss check task started");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));  // Check every 10 seconds
        
        // Only for non-root nodes
        if (g_is_root) {
            continue;
        }
        
        uint32_t current_time = esp_timer_get_time() / 1000;  // ms
        
        // Check if we're connected to mesh
        if (g_mesh_connected) {
            // Update last contact time
            g_last_root_contact = current_time;
        } else {
            // Check if we've been disconnected too long
            uint32_t time_since_contact = current_time - g_last_root_contact;
            
            if (time_since_contact > ROOT_LOSS_RESET_TIMEOUT_MS) {
                ESP_LOGE(TAG, "Root lost for %lu ms, resetting device...", time_since_contact);
                
                // Track disconnect
                if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    g_stats.mesh_disconnects++;
                    xSemaphoreGive(g_stats_mutex);
                }
                
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            } else if (time_since_contact > 60000) {
                // Log warning every minute
                if ((time_since_contact / 60000) != ((time_since_contact - 10000) / 60000)) {
                    ESP_LOGW(TAG, "Root connection lost for %lu seconds", time_since_contact / 1000);
                }
            }
        }
    }
}
