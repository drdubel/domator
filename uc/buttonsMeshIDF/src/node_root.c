#include "domator_mesh.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "NODE_ROOT";

// ====================
// MQTT Event Handler
// ====================

void mqtt_event_handler(void *handler_args, esp_event_base_t base, 
                        int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            g_mqtt_connected = true;
            
            // Subscribe to command topics
            esp_mqtt_client_subscribe(g_mqtt_client, "/switch/cmd/+", 0);
            esp_mqtt_client_subscribe(g_mqtt_client, "/switch/cmd", 0);
            esp_mqtt_client_subscribe(g_mqtt_client, "/relay/cmd/+", 0);
            esp_mqtt_client_subscribe(g_mqtt_client, "/relay/cmd", 0);
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            g_mqtt_connected = false;
            break;
            
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT subscribed, msg_id=%d", event->msg_id);
            break;
            
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT data received: topic=%.*s, data=%.*s",
                     event->topic_len, event->topic,
                     event->data_len, event->data);
            // TODO: Handle MQTT commands
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;
            
        default:
            ESP_LOGD(TAG, "MQTT event: %d", event_id);
            break;
    }
}

// ====================
// MQTT Initialization
// ====================

void mqtt_init(void)
{
    if (!g_is_root) {
        ESP_LOGW(TAG, "Not root node, skipping MQTT init");
        return;
    }
    
    ESP_LOGI(TAG, "Initializing MQTT client");
    
    // Build complete MQTT broker URI
    char broker_uri[128];
    const char *url = CONFIG_MQTT_BROKER_URL;
    
    // Check if URL already includes port
    if (strstr(url, "mqtt://") != NULL && strchr(url + 7, ':') == NULL) {
        // No port in URL, append it
        snprintf(broker_uri, sizeof(broker_uri), "%s:%d", url, CONFIG_MQTT_BROKER_PORT);
    } else {
        // URL already complete or has port
        snprintf(broker_uri, sizeof(broker_uri), "%s", url);
    }
    
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.username = CONFIG_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
    };
    
    g_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (g_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }
    
    esp_mqtt_client_register_event(g_mqtt_client, ESP_EVENT_ANY_ID,
                                    mqtt_event_handler, NULL);
    
    esp_err_t err = esp_mqtt_client_start(g_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "MQTT client started");
    }
}

// ====================
// Root Publish Status
// ====================

void root_publish_status(void)
{
    if (!g_is_root || !g_mqtt_connected || g_mqtt_client == NULL) {
        return;
    }
    
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return;
    }
    
    // Get uptime in seconds
    uint32_t uptime = esp_timer_get_time() / 1000000;
    
    // Get free heap
    uint32_t free_heap = esp_get_free_heap_size();
    
    // Check for low heap
    if (free_heap < LOW_HEAP_THRESHOLD) {
        if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_stats.low_heap_events++;
            xSemaphoreGive(g_stats_mutex);
        }
    }
    
    // Get peer count (number of connected children)
    int peer_count = esp_mesh_get_total_node_num() - 1;  // Subtract self
    
    // Build JSON status report
    cJSON_AddNumberToObject(json, "deviceId", g_device_id);
    cJSON_AddNumberToObject(json, "parentId", g_device_id);  // Root's parent is itself
    cJSON_AddStringToObject(json, "type", "root");
    cJSON_AddNumberToObject(json, "freeHeap", free_heap);
    cJSON_AddNumberToObject(json, "uptime", uptime);
    cJSON_AddNumberToObject(json, "meshLayer", g_mesh_layer);
    cJSON_AddNumberToObject(json, "peerCount", peer_count);
    cJSON_AddStringToObject(json, "firmware", g_firmware_hash);
    cJSON_AddNumberToObject(json, "clicks", g_stats.button_presses);
    cJSON_AddNumberToObject(json, "lowHeap", g_stats.low_heap_events);
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    if (json_str != NULL) {
        int msg_id = esp_mqtt_client_publish(g_mqtt_client, "/switch/state/root",
                                             json_str, 0, 0, 0);
        if (msg_id >= 0) {
            ESP_LOGD(TAG, "Published root status: %s", json_str);
        } else {
            ESP_LOGW(TAG, "Failed to publish root status");
            
            if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_stats.mqtt_dropped++;
                xSemaphoreGive(g_stats_mutex);
            }
        }
        
        free(json_str);
    }
}

// ====================
// Forward Leaf Status to MQTT
// ====================

void root_forward_leaf_status(const char *json_str)
{
    if (!g_is_root || !g_mqtt_connected || g_mqtt_client == NULL) {
        return;
    }
    
    // Parse the JSON to add parentId
    cJSON *json = cJSON_Parse(json_str);
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse leaf status JSON");
        return;
    }
    
    // Add parentId (root's device ID)
    cJSON_AddNumberToObject(json, "parentId", g_device_id);
    
    char *modified_json = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    if (modified_json != NULL) {
        int msg_id = esp_mqtt_client_publish(g_mqtt_client, "/switch/state/root",
                                             modified_json, 0, 0, 0);
        if (msg_id >= 0) {
            ESP_LOGD(TAG, "Forwarded leaf status: %s", modified_json);
        } else {
            ESP_LOGW(TAG, "Failed to forward leaf status");
            
            if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_stats.mqtt_dropped++;
                xSemaphoreGive(g_stats_mutex);
            }
        }
        
        free(modified_json);
    }
}

// ====================
// Handle Mesh Message (Root Only)
// ====================

void root_handle_mesh_message(const mesh_addr_t *from, const mesh_app_msg_t *msg)
{
    if (!g_is_root) {
        return;
    }
    
    ESP_LOGD(TAG, "Processing message type '%c' from device %" PRIu32,
             msg->msg_type, msg->device_id);
    
    switch (msg->msg_type) {
        case MSG_TYPE_BUTTON: {
            // Button press from switch node
            if (msg->data_len > 0 && msg->data_len < sizeof(msg->data)) {
                char button_char = msg->data[0];
                
                ESP_LOGI(TAG, "Button '%c' pressed on device %" PRIu32,
                         button_char, msg->device_id);
                
                // Publish to MQTT: /switch/state/{deviceId}
                if (g_mqtt_connected) {
                    char topic[64];
                    snprintf(topic, sizeof(topic), "/switch/state/%" PRIu32, msg->device_id);
                    
                    char payload[2] = {button_char, '\0'};
                    
                    int msg_id = esp_mqtt_client_publish(g_mqtt_client, topic,
                                                         payload, 1, 0, 0);
                    if (msg_id >= 0) {
                        ESP_LOGD(TAG, "Published button press to %s: %c", topic, button_char);
                    } else {
                        ESP_LOGW(TAG, "Failed to publish button press");
                        
                        if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            g_stats.mqtt_dropped++;
                            xSemaphoreGive(g_stats_mutex);
                        }
                    }
                }
            }
            break;
        }
        
        case MSG_TYPE_STATUS: {
            // Status report from leaf node
            if (msg->data_len > 0 && msg->data_len < sizeof(msg->data)) {
                // Ensure null termination
                char json_str[sizeof(msg->data) + 1];
                memcpy(json_str, msg->data, msg->data_len);
                json_str[msg->data_len] = '\0';
                
                ESP_LOGD(TAG, "Received status from device %" PRIu32 ": %s",
                         msg->device_id, json_str);
                
                // Forward to MQTT with parentId added
                root_forward_leaf_status(json_str);
            }
            break;
        }
        
        case MSG_TYPE_COMMAND:
            // Command from another node (future use)
            ESP_LOGD(TAG, "Command message received");
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown message type: '%c'", msg->msg_type);
            break;
    }
}
