#include "domator_mesh.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "cJSON.h"

static const char *TAG = "MESH_COMM";

// ====================
// Queue Message to Root
// ====================

esp_err_t mesh_queue_to_root(const mesh_app_msg_t *msg)
{
    if (g_mesh_tx_queue == NULL) {
        ESP_LOGE(TAG, "TX queue not initialized");
        return ESP_FAIL;
    }
    
    if (xQueueSend(g_mesh_tx_queue, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "TX queue full, dropping message type '%c'", msg->msg_type);
        
        if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            g_stats.mesh_send_failed++;
            xSemaphoreGive(g_stats_mutex);
        }
        
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// ====================
// Mesh Send Task
// ====================

void mesh_send_task(void *arg)
{
    mesh_app_msg_t msg;
    mesh_data_t data;
    
    ESP_LOGI(TAG, "Mesh send task started");
    
    while (1) {
        // Wait for messages in queue
        if (xQueueReceive(g_mesh_tx_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            
            // Prepare mesh data
            data.data = (uint8_t *)&msg;
            data.size = sizeof(mesh_app_msg_t);
            data.proto = MESH_PROTO_BIN;
            data.tos = MESH_TOS_P2P;
            
            esp_err_t err;
            
            if (g_is_root) {
                // Root node - this shouldn't happen normally as root doesn't send to root
                ESP_LOGW(TAG, "Root trying to send to itself");
                continue;
            } else {
                // Send to root
                err = esp_mesh_send(NULL, &data, MESH_DATA_P2P, NULL, 0);
            }
            
            if (err == ESP_OK) {
                ESP_LOGD(TAG, "Sent message type '%c' to root", msg.msg_type);
                
                if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    g_stats.mesh_send_success++;
                    xSemaphoreGive(g_stats_mutex);
                }
            } else {
                ESP_LOGW(TAG, "Failed to send message type '%c': %s", 
                         msg.msg_type, esp_err_to_name(err));
                
                if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    g_stats.mesh_send_failed++;
                    xSemaphoreGive(g_stats_mutex);
                }
            }
        }
        
        // Small delay to prevent tight loop
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ====================
// Handle Received Message
// ====================

void handle_mesh_recv(const mesh_addr_t *from, const mesh_app_msg_t *msg)
{
    ESP_LOGD(TAG, "Received message type '%c' from device %" PRIu32, 
             msg->msg_type, msg->device_id);
    
    // If this is root node, handle the message
    if (g_is_root) {
        root_handle_mesh_message(from, msg);
    } else {
        // Non-root nodes typically don't receive messages from mesh
        ESP_LOGD(TAG, "Non-root node received mesh message");
    }
}

// ====================
// Mesh Receive Task
// ====================

void mesh_recv_task(void *arg)
{
    mesh_data_t data;
    mesh_addr_t from;
    int flag = 0;
    
    ESP_LOGI(TAG, "Mesh receive task started");
    
    // Allocate receive buffer
    data.data = malloc(MESH_MPS);
    if (data.data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate receive buffer");
        vTaskDelete(NULL);
        return;
    }
    data.size = MESH_MPS;
    
    while (1) {
        // Receive mesh data
        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Mesh receive error: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // Check if it's our application message
        if (data.size >= sizeof(mesh_app_msg_t)) {
            mesh_app_msg_t *msg = (mesh_app_msg_t *)data.data;
            handle_mesh_recv(&from, msg);
        } else {
            ESP_LOGW(TAG, "Received message too small: %d bytes", data.size);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    free(data.data);
}

// ====================
// Status Report Task
// ====================

void status_report_task(void *arg)
{
    ESP_LOGI(TAG, "Status report task started");
    
    // Wait for mesh to stabilize
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    while (1) {
        if (g_ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        if (g_is_root) {
            // Root node publishes its own status via MQTT
            root_publish_status();
        } else {
            // Leaf nodes send status to root via mesh
            if (g_mesh_connected) {
                cJSON *json = cJSON_CreateObject();
                if (json == NULL) {
                    ESP_LOGE(TAG, "Failed to create JSON object");
                    vTaskDelay(pdMS_TO_TICKS(STATUS_REPORT_INTERVAL_MS));
                    continue;
                }
                
                // Get uptime in seconds
                uint32_t uptime = esp_timer_get_time() / 1000000;
                
                // Get free heap
                uint32_t free_heap = esp_get_free_heap_size();
                
                // Get RSSI from parent connection
                int8_t rssi = 0;
                esp_wifi_sta_get_rssi(&rssi);
                
                // Check for low heap
                if (free_heap < LOW_HEAP_THRESHOLD) {
                    if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        g_stats.low_heap_events++;
                        xSemaphoreGive(g_stats_mutex);
                    }
                }
                
                // Build JSON status report
                cJSON_AddNumberToObject(json, "deviceId", g_device_id);
                cJSON_AddStringToObject(json, "type", "switch");
                cJSON_AddNumberToObject(json, "freeHeap", free_heap);
                cJSON_AddNumberToObject(json, "uptime", uptime);
                cJSON_AddStringToObject(json, "firmware", g_firmware_hash);
                cJSON_AddNumberToObject(json, "clicks", g_stats.button_presses);
                cJSON_AddNumberToObject(json, "rssi", rssi);
                cJSON_AddNumberToObject(json, "disconnects", g_stats.mesh_disconnects);
                cJSON_AddNumberToObject(json, "lowHeap", g_stats.low_heap_events);
                
                char *json_str = cJSON_PrintUnformatted(json);
                cJSON_Delete(json);
                
                if (json_str != NULL) {
                    // Queue status message to root
                    mesh_app_msg_t msg = {0};
                    msg.msg_type = MSG_TYPE_STATUS;
                    msg.device_id = g_device_id;
                    msg.data_len = strlen(json_str);
                    
                    if (msg.data_len < sizeof(msg.data) - 1) {
                        memcpy(msg.data, json_str, msg.data_len);
                        msg.data[msg.data_len] = '\0';
                        
                        esp_err_t err = mesh_queue_to_root(&msg);
                        if (err == ESP_OK) {
                            ESP_LOGD(TAG, "Queued status report: %s", json_str);
                        }
                    }
                    
                    free(json_str);
                }
            } else {
                ESP_LOGD(TAG, "Not connected to mesh, skipping status report");
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(STATUS_REPORT_INTERVAL_MS));
    }
}
