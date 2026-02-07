#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "cJSON.h"
#include "domator_mesh.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"

static const char* TAG = "NODE_ROOT";

// ====================
// Helper Functions
// ====================

/**
 * Parse device ID from string representation
 * @param device_id_str String representation of device ID (e.g., "12345678")
 * @return Parsed device ID as uint32_t, or 0 if parsing fails
 */
static uint32_t parse_device_id_from_string(const char *device_id_str) {
    if (device_id_str == NULL) {
        ESP_LOGW(TAG, "parse_device_id_from_string: NULL input");
        return 0;
    }
    
    char *endptr;
    errno = 0;  // Reset errno before strtoul
    unsigned long parsed = strtoul(device_id_str, &endptr, 10);
    
    // Check for overflow
    if (errno == ERANGE) {
        ESP_LOGW(TAG, "parse_device_id_from_string: Value overflow in '%s'", device_id_str);
        return 0;
    }
    
    // Check if parsing was successful and string was fully consumed
    if (*endptr != '\0') {
        ESP_LOGW(TAG, "parse_device_id_from_string: Invalid format '%s'", device_id_str);
        return 0;
    }
    
    // Check if value is 0 (used as error sentinel in codebase)
    if (parsed == 0) {
        ESP_LOGW(TAG, "parse_device_id_from_string: Device ID 0 is invalid");
        return 0;
    }
    
    // Check for overflow (device IDs must fit in uint32_t)
    // On 64-bit platforms, unsigned long may be larger than uint32_t
    if (parsed > UINT32_MAX) {
        ESP_LOGW(TAG, "parse_device_id_from_string: Value %lu exceeds UINT32_MAX", parsed);
        return 0;
    }
    
    return (uint32_t)parsed;
}

/**
 * Convert button character to array index
 * Handles gesture characters:
 * - Single press: 'a'-'g' (indices 0-6)
 * - Double press: 'h'-'n' (indices 0-6, mapped from h=0, i=1, ..., n=6)
 * - Long press: 'o'-'u' (indices 0-6, mapped from o=0, p=1, ..., u=6)
 * - Extended buttons: 'a'-'p' for 16-button support (indices 0-15)
 * @param button_char Button character
 * @return Button index (0-15), or -1 if invalid
 */
static int button_char_to_index(char button_char) {
    // Single press or extended buttons: 'a'-'p' → 0-15
    if (button_char >= 'a' && button_char <= 'p') {
        return button_char - 'a';
    }
    if (button_char >= 'A' && button_char <= 'P') {
        return button_char - 'A';
    }
    
    // Double press: 'h'-'n' → map back to base button 0-6
    // h=button0, i=button1, ..., n=button6
    if (button_char >= 'h' && button_char <= 'n') {
        return button_char - 'h';  // 0-6
    }
    if (button_char >= 'H' && button_char <= 'N') {
        return button_char - 'H';  // 0-6
    }
    
    // Long press: 'o'-'u' → map back to base button 0-6
    // o=button0, p=button1, ..., u=button6
    if (button_char >= 'o' && button_char <= 'u') {
        return button_char - 'o';  // 0-6
    }
    if (button_char >= 'O' && button_char <= 'U') {
        return button_char - 'O';  // 0-6
    }
    
    return -1;
}

/**
 * Find device MAC address by device ID
 * @param device_id Device ID to look up
 * @param mac_addr Output: MAC address if found
 * @return true if device found, false otherwise
 */
static bool find_device_mac(uint32_t device_id, mesh_addr_t *mac_addr) {
    if (mac_addr == NULL) {
        return false;
    }
    
    // Search in peer health tracking table
    for (uint8_t i = 0; i < g_peer_count; i++) {
        if (g_peer_health[i].device_id == device_id && g_peer_health[i].is_alive) {
            memcpy(mac_addr, &g_peer_health[i].mac_addr, sizeof(mesh_addr_t));
            return true;
        }
    }
    
    return false;
}

// ====================
// MQTT Command Handling
// ====================

static void handle_mqtt_command(const char* topic, int topic_len,
                                const char* data, int data_len) {
    if (topic == NULL || data == NULL || topic_len == 0 || data_len == 0) {
        return;
    }

    // Extract topic string
    char topic_str[128];
    int copy_len =
        (topic_len < sizeof(topic_str) - 1) ? topic_len : sizeof(topic_str) - 1;
    memcpy(topic_str, topic, copy_len);
    topic_str[copy_len] = '\0';

    ESP_LOGI(TAG, "Processing MQTT command: topic=%s", topic_str);

    // Check if this is a configuration message for root
    if (strstr(topic_str, "/switch/cmd/root") != NULL) {
        // Try to parse as JSON configuration
        if (data[0] == '{') {
            // Looks like JSON, try to parse it
            cJSON *json = cJSON_Parse(data);
            if (json != NULL) {
                cJSON *type_item = cJSON_GetObjectItem(json, "type");
                if (type_item != NULL && cJSON_IsString(type_item)) {
                    const char *config_type = type_item->valuestring;
                    
                    if (strcmp(config_type, "connections") == 0) {
                        ESP_LOGI(TAG, "Received connections configuration");
                        root_parse_connections(data);
                    } else if (strcmp(config_type, "button_types") == 0) {
                        ESP_LOGI(TAG, "Received button types configuration");
                        root_parse_button_types(data);
                    } else if (strcmp(config_type, "gesture_config") == 0) {
                        ESP_LOGI(TAG, "Received gesture configuration for switch nodes");
                        // Forward gesture config to all switch nodes
                        cJSON *target_device_item = cJSON_GetObjectItem(json, "device_id");
                        if (target_device_item != NULL && cJSON_IsString(target_device_item)) {
                            const char *target_device_str = target_device_item->valuestring;
                            uint32_t target_device = parse_device_id_from_string(target_device_str);
                            
                            if (target_device != 0) {
                                ESP_LOGI(TAG, "Forwarding gesture config to device %" PRIu32, target_device);
                                
                                // Create mesh config message
                                mesh_app_msg_t msg = {0};
                                msg.msg_type = MSG_TYPE_CONFIG;
                                msg.device_id = g_device_id;  // From root
                                
                                // Copy JSON data
                                size_t json_len = strlen(data);
                                if (json_len >= MESH_MSG_DATA_SIZE) {
                                    json_len = MESH_MSG_DATA_SIZE - 1;
                                    ESP_LOGW(TAG, "Gesture config truncated to %d bytes", MESH_MSG_DATA_SIZE - 1);
                                }
                                memcpy(msg.data, data, json_len);
                                msg.data[json_len] = '\0';
                                msg.data_len = json_len;
                                
                                // Broadcast to all nodes (they filter by their device ID)
                                mesh_data_t mdata;
                                mdata.data = (uint8_t*)&msg;
                                mdata.size = sizeof(mesh_app_msg_t);
                                mdata.proto = MESH_PROTO_BIN;
                                mdata.tos = MESH_TOS_P2P;
                                
                                esp_mesh_send(NULL, &mdata, MESH_DATA_P2P, NULL, 0);
                                ESP_LOGI(TAG, "Gesture config broadcasted to mesh");
                            } else {
                                ESP_LOGW(TAG, "Invalid target device ID in gesture config");
                            }
                        } else {
                            ESP_LOGW(TAG, "No device_id in gesture config, cannot forward");
                        }
                    } else if (strcmp(config_type, "ota_trigger") == 0) {
                        ESP_LOGI(TAG, "Received OTA trigger configuration");
                        // Forward OTA trigger to all nodes
                        cJSON *url_item = cJSON_GetObjectItem(json, "url");
                        if (url_item != NULL && cJSON_IsString(url_item)) {
                            const char *ota_url = url_item->valuestring;
                            
                            ESP_LOGI(TAG, "Broadcasting OTA trigger: %s", ota_url);
                            
                            // Create OTA trigger message
                            mesh_app_msg_t msg = {0};
                            msg.msg_type = MSG_TYPE_OTA_TRIGGER;
                            msg.device_id = g_device_id;
                            
                            size_t url_len = strlen(ota_url);
                            if (url_len >= MESH_MSG_DATA_SIZE) {
                                url_len = MESH_MSG_DATA_SIZE - 1;
                            }
                            memcpy(msg.data, ota_url, url_len);
                            msg.data[url_len] = '\0';
                            msg.data_len = url_len;
                            
                            // Broadcast to all nodes
                            mesh_data_t mdata;
                            mdata.data = (uint8_t*)&msg;
                            mdata.size = sizeof(mesh_app_msg_t);
                            mdata.proto = MESH_PROTO_BIN;
                            mdata.tos = MESH_TOS_P2P;
                            
                            esp_mesh_send(NULL, &mdata, MESH_DATA_P2P, NULL, 0);
                            ESP_LOGI(TAG, "OTA trigger broadcasted to mesh");
                        }
                    } else {
                        ESP_LOGW(TAG, "Unknown config type: %s", config_type);
                    }
                } else {
                    ESP_LOGW(TAG, "JSON missing 'type' field");
                }
                cJSON_Delete(json);
            } else {
                ESP_LOGW(TAG, "Failed to parse JSON configuration");
            }
            return;  // Configuration handled, return early
        }
    }

    // Extract command string for non-JSON commands
    char cmd_str[64];
    copy_len =
        (data_len < sizeof(cmd_str) - 1) ? data_len : sizeof(cmd_str) - 1;
    memcpy(cmd_str, data, copy_len);
    cmd_str[copy_len] = '\0';

    ESP_LOGI(TAG, "Processing MQTT command: topic=%s, cmd=%s", topic_str,
             cmd_str);

    // Determine if this is a relay or switch command
    bool is_relay_cmd = (strstr(topic_str, "/relay/cmd") != NULL);
    bool is_switch_cmd = (strstr(topic_str, "/switch/cmd") != NULL);

    if (!is_relay_cmd && !is_switch_cmd) {
        ESP_LOGW(TAG, "Unknown command topic: %s", topic_str);
        return;
    }

    // Extract device ID if present (e.g., /relay/cmd/12345678)
    uint32_t target_device_id = 0;
    char* last_slash = strrchr(topic_str, '/');
    if (last_slash != NULL && last_slash[1] != '\0') {
        // Try to parse as device ID
        char* endptr;
        unsigned long parsed = strtoul(last_slash + 1, &endptr, 10);
        if (*endptr == '\0' && parsed > 0) {
            target_device_id = (uint32_t)parsed;
        }
    }

    // Create mesh command message
    mesh_app_msg_t msg = {0};
    msg.msg_type = MSG_TYPE_COMMAND;
    msg.device_id = g_device_id;  // From root

    // Copy command data
    size_t cmd_len = strlen(cmd_str);
    if (cmd_len >= MESH_MSG_DATA_SIZE) {
        cmd_len = MESH_MSG_DATA_SIZE - 1;
    }
    memcpy(msg.data, cmd_str, cmd_len);
    msg.data[cmd_len] = '\0';
    msg.data_len = cmd_len;

    // Prepare mesh_data_t wrapper for sending
    mesh_data_t mdata;
    mdata.data = (uint8_t*)&msg;
    mdata.size = sizeof(mesh_app_msg_t);
    mdata.proto = MESH_PROTO_BIN;
    mdata.tos = MESH_TOS_P2P;

    // Attempt device-specific targeting if device ID provided
    if (target_device_id != 0) {
        mesh_addr_t target_mac;
        
        // Try to find device MAC address
        if (find_device_mac(target_device_id, &target_mac)) {
            // Found device MAC - send directly to target device
            ESP_LOGI(TAG, "Sending command to device %" PRIu32 " (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
                     target_device_id,
                     target_mac.addr[0], target_mac.addr[1], target_mac.addr[2],
                     target_mac.addr[3], target_mac.addr[4], target_mac.addr[5]);
            
            esp_err_t err = esp_mesh_send(&target_mac, &mdata, MESH_DATA_P2P, NULL, 0);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send to device %" PRIu32 ": %s, broadcasting instead",
                         target_device_id, esp_err_to_name(err));
                esp_mesh_send(NULL, &mdata, MESH_DATA_P2P, NULL, 0);
            }
        } else {
            // Device not found in tracking table - broadcast as fallback
            ESP_LOGW(TAG, "Device %" PRIu32 " not found in tracking table, broadcasting",
                     target_device_id);
            ESP_LOGI(TAG, "Broadcasting command to all nodes (intended for %" PRIu32 ")",
                     target_device_id);
            esp_mesh_send(NULL, &mdata, MESH_DATA_P2P, NULL, 0);
        }
    } else {
        // No specific device ID - broadcast to all nodes
        ESP_LOGI(TAG, "Broadcasting command to all nodes");
        esp_mesh_send(NULL, &mdata, MESH_DATA_P2P, NULL, 0);
    }
}

// ====================
// MQTT Connection Status
// ====================

/**
 * Publish connection status message to MQTT
 * @param connected true for connected, false for disconnected
 */
static void publish_connection_status(bool connected) {
    if (!g_mqtt_client) {
        return;
    }

    // Create JSON status message
    cJSON* json = cJSON_CreateObject();
    if (!json) {
        ESP_LOGE(TAG, "Failed to create connection status JSON");
        return;
    }

    cJSON_AddStringToObject(json, "status", connected ? "connected" : "disconnected");
    cJSON_AddNumberToObject(json, "device_id", g_device_id);
    cJSON_AddNumberToObject(json, "timestamp", (double)(esp_timer_get_time() / 1000000));
    
    if (connected) {
        // Add additional info on connection
        extern const char* g_firmware_version;
        cJSON_AddStringToObject(json, "firmware", g_firmware_version);
        cJSON_AddNumberToObject(json, "mesh_layer", g_mesh_layer);
        
        // Get IP address if available
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                char ip_str[16];
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                cJSON_AddStringToObject(json, "ip", ip_str);
            }
        }
    }

    char* json_str = cJSON_PrintUnformatted(json);
    if (json_str) {
        // Publish with QoS 1 and retain flag for monitoring
        int msg_id = esp_mqtt_client_publish(g_mqtt_client, "/status/root/connection",
                                              json_str, 0, 1, 1);
        if (msg_id >= 0) {
            ESP_LOGI(TAG, "Published connection status: %s (msg_id=%d)", 
                     connected ? "connected" : "disconnected", msg_id);
        } else {
            ESP_LOGE(TAG, "Failed to publish connection status");
        }
        free(json_str);
    }
    
    cJSON_Delete(json);
}

// ====================
// MQTT Event Handler
// ====================

void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                        int32_t event_id, void* event_data) {
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
            esp_mqtt_client_subscribe(g_mqtt_client, "/switch/cmd/root", 0);  // For config
            
            // Publish connection status
            publish_connection_status(true);
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
                     event->topic_len, event->topic, event->data_len,
                     event->data);
            handle_mqtt_command(event->topic, event->topic_len, event->data,
                                event->data_len);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;

        default:
            ESP_LOGD(TAG, "MQTT event: %" PRId32, event_id);
            break;
    }
}

// ====================
// MQTT Initialization
// ====================

void mqtt_init(void) {
    if (!g_is_root) {
        ESP_LOGW(TAG, "Not root node, skipping MQTT init");
        return;
    }

    ESP_LOGI(TAG, "Initializing MQTT client");

    // Build complete MQTT broker URI
    char broker_uri[128];
    const char* url = CONFIG_MQTT_BROKER_URL;

    // Check if URL already includes port
    if (strstr(url, "mqtt://") != NULL && strchr(url + 7, ':') == NULL) {
        // No port in URL, append it
        snprintf(broker_uri, sizeof(broker_uri), "%s:%d", url,
                 CONFIG_MQTT_BROKER_PORT);
    } else {
        // URL already complete or has port
        snprintf(broker_uri, sizeof(broker_uri), "%s", url);
    }

    // Prepare Last Will and Testament (LWT) message for ungraceful disconnects
    char lwt_message[256];
    snprintf(lwt_message, sizeof(lwt_message),
             "{\"status\":\"disconnected\",\"device_id\":%" PRIu32 ",\"timestamp\":%" PRId64 ",\"reason\":\"ungraceful\"}",
             g_device_id, esp_timer_get_time() / 1000000);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.username = CONFIG_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
        .session.last_will = {
            .topic = "/status/root/connection",
            .msg = lwt_message,
            .msg_len = strlen(lwt_message),
            .qos = 1,
            .retain = 1,
        },
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

void root_publish_status(void) {
    if (!g_is_root || !g_mqtt_connected || g_mqtt_client == NULL) {
        return;
    }

    cJSON* json = cJSON_CreateObject();
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
        if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(STATS_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            g_stats.low_heap_events++;
            xSemaphoreGive(g_stats_mutex);
        }
    }

    // Get peer count (number of connected children)
    int peer_count = esp_mesh_get_total_node_num() - 1;  // Subtract self

    // Build JSON status report
    cJSON_AddNumberToObject(json, "deviceId", g_device_id);
    cJSON_AddNumberToObject(json, "parentId",
                            g_device_id);  // Root's parent is itself
    cJSON_AddStringToObject(json, "type", "root");
    cJSON_AddNumberToObject(json, "freeHeap", free_heap);
    cJSON_AddNumberToObject(json, "uptime", uptime);
    cJSON_AddNumberToObject(json, "meshLayer", g_mesh_layer);
    cJSON_AddNumberToObject(json, "peerCount", peer_count);
    cJSON_AddStringToObject(json, "firmware", g_firmware_hash);
    cJSON_AddNumberToObject(json, "clicks", g_stats.button_presses);
    cJSON_AddNumberToObject(json, "lowHeap", g_stats.low_heap_events);

    char* json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (json_str != NULL) {
        int msg_id = esp_mqtt_client_publish(
            g_mqtt_client, "/switch/state/root", json_str, 0, 0, 0);
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

void root_forward_leaf_status(const char* json_str) {
    if (!g_is_root || !g_mqtt_connected || g_mqtt_client == NULL) {
        return;
    }

    // Parse the JSON to add parentId
    cJSON* json = cJSON_Parse(json_str);
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse leaf status JSON");
        return;
    }

    // Add parentId (root's device ID)
    cJSON_AddNumberToObject(json, "parentId", g_device_id);

    char* modified_json = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (modified_json != NULL) {
        int msg_id = esp_mqtt_client_publish(
            g_mqtt_client, "/switch/state/root", modified_json, 0, 0, 0);
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

void root_handle_mesh_message(const mesh_addr_t* from,
                              const mesh_app_msg_t* msg) {
    if (!g_is_root) {
        return;
    }

    ESP_LOGD(TAG, "Processing message type '%c' from device %" PRIu32,
             msg->msg_type, msg->device_id);
    
    // Update peer health tracking with MAC address
    // Note: RSSI not available from mesh_addr_t, using 0 as placeholder
    peer_health_update(msg->device_id, from, 0);

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
                    snprintf(topic, sizeof(topic), "/switch/state/%" PRIu32,
                             msg->device_id);

                    char payload[2] = {button_char, '\0'};

                    int msg_id = esp_mqtt_client_publish(g_mqtt_client, topic,
                                                         payload, 1, 0, 0);
                    if (msg_id >= 0) {
                        ESP_LOGD(TAG, "Published button press to %s: %c", topic,
                                 button_char);
                    } else {
                        ESP_LOGW(TAG, "Failed to publish button press");

                        if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(10)) ==
                            pdTRUE) {
                            g_stats.mqtt_dropped++;
                            xSemaphoreGive(g_stats_mutex);
                        }
                    }
                }
                
                // Route button press to configured relay targets (#15)
                // Note: Pass 1 to indicate button press. For toggle buttons (type 0),
                // state is ignored. For stateful buttons (type 1), this sends state=1.
                root_route_button_press(msg->device_id, button_char, 1);
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

        case MSG_TYPE_RELAY_STATE: {
            // Relay state confirmation from relay node
            if (msg->data_len > 0 && msg->data_len < sizeof(msg->data)) {
                // Ensure null termination
                char state_str[sizeof(msg->data) + 1];
                memcpy(state_str, msg->data, msg->data_len);
                state_str[msg->data_len] = '\0';

                ESP_LOGI(TAG, "Relay state from device %" PRIu32 ": %s",
                         msg->device_id, state_str);

                // Publish to MQTT: /relay/state/{deviceId}/{relay}
                // Format: state_str should be like "a0" (relay a is OFF) or
                // "a1" (relay a is ON)
                if (g_mqtt_connected && strlen(state_str) >= 2) {
                    char topic[64];
                    char relay_char = state_str[0];
                    char state_char = state_str[1];

                    snprintf(topic, sizeof(topic),
                             "/relay/state/%" PRIu32 "/%c", msg->device_id,
                             relay_char);

                    char payload[2] = {state_char, '\0'};

                    int mqtt_msg_id = esp_mqtt_client_publish(
                        g_mqtt_client, topic, payload, 1, 0, 0);
                    if (mqtt_msg_id >= 0) {
                        ESP_LOGD(TAG, "Published relay state to %s: %c", topic,
                                 state_char);
                    } else {
                        ESP_LOGW(TAG, "Failed to publish relay state");

                        if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(10)) ==
                            pdTRUE) {
                            g_stats.mqtt_dropped++;
                            xSemaphoreGive(g_stats_mutex);
                        }
                    }
                }
            }
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown message type: '%c'", msg->msg_type);
            break;
    }
}

// ====================
// Routing Initialization
// ====================

void root_init_routing(void)
{
    ESP_LOGI(TAG, "Initializing routing tables");
    
    // Clear all routing data
    memset(g_connections, 0, sizeof(g_connections));
    memset(g_device_ids, 0, sizeof(g_device_ids));
    memset(g_button_types, 0, sizeof(g_button_types));
    g_num_devices = 0;
    
    ESP_LOGI(TAG, "Routing tables initialized");
}

// ====================
// Device Index Helper
// ====================

int root_find_device_index(uint32_t device_id)
{
    for (uint8_t i = 0; i < g_num_devices; i++) {
        if (g_device_ids[i] == device_id) {
            return i;
        }
    }
    
    // Add new device if there's space
    if (g_num_devices < MAX_DEVICES) {
        g_device_ids[g_num_devices] = device_id;
        g_num_devices++;
        ESP_LOGI(TAG, "Added device %" PRIu32 " at index %d", device_id, g_num_devices - 1);
        return g_num_devices - 1;
    }
    
    ESP_LOGW(TAG, "Max devices reached, cannot add device %" PRIu32, device_id);
    return -1;
}

// ====================
// Connection Map Parsing (#13)
// ====================

void root_parse_connections(const char *json_str)
{
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Connection JSON is NULL");
        return;
    }
    
    ESP_LOGI(TAG, "Parsing connections configuration");
    
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse connections JSON");
        return;
    }
    
    if (xSemaphoreTake(g_connections_mutex, pdMS_TO_TICKS(ROUTING_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire connections mutex");
        cJSON_Delete(root);
        return;
    }
    
    // Clear existing connections
    for (uint8_t i = 0; i < MAX_DEVICES; i++) {
        for (uint8_t b = 0; b < 16; b++) {
            if (g_connections[i].buttons[b].targets != NULL) {
                free(g_connections[i].buttons[b].targets);
                g_connections[i].buttons[b].targets = NULL;
            }
            g_connections[i].buttons[b].num_targets = 0;
        }
    }
    
    // Parse JSON: { "deviceId": { "button": [["targetId", "command"], ...], ... }, ... }
    cJSON *device_item = NULL;
    cJSON_ArrayForEach(device_item, root) {
        const char *device_id_str = device_item->string;
        uint32_t device_id = parse_device_id_from_string(device_id_str);
        
        int dev_idx = root_find_device_index(device_id);
        if (dev_idx < 0) {
            continue;
        }
        
        // Iterate through buttons for this device
        cJSON *button_item = NULL;
        cJSON_ArrayForEach(button_item, device_item) {
            const char *button_str = button_item->string;
            if (strlen(button_str) != 1) continue;
            
            char button_char = button_str[0];
            int button_idx = button_char_to_index(button_char);
            
            if (button_idx < 0 || button_idx >= 16) continue;
            
            // Count targets
            int num_targets = cJSON_GetArraySize(button_item);
            if (num_targets == 0 || num_targets > MAX_ROUTES_PER_BUTTON) {
                continue;
            }
            
            // Allocate targets array
            route_target_t *targets = (route_target_t *)malloc(sizeof(route_target_t) * num_targets);
            if (targets == NULL) {
                ESP_LOGE(TAG, "Failed to allocate targets for device %" PRIu32 " button %c",
                         device_id, button_char);
                continue;
            }
            
            // Parse target array
            int target_count = 0;
            cJSON *target_item = NULL;
            cJSON_ArrayForEach(target_item, button_item) {
                if (!cJSON_IsArray(target_item) || cJSON_GetArraySize(target_item) < 2) {
                    continue;
                }
                
                cJSON *target_id_item = cJSON_GetArrayItem(target_item, 0);
                cJSON *command_item = cJSON_GetArrayItem(target_item, 1);
                
                if (target_id_item == NULL || command_item == NULL) {
                    continue;
                }
                
                uint32_t target_id = 0;
                if (cJSON_IsString(target_id_item)) {
                    target_id = parse_device_id_from_string(target_id_item->valuestring);
                } else if (cJSON_IsNumber(target_id_item)) {
                    target_id = (uint32_t)target_id_item->valueint;
                }
                
                if (target_id == 0 || !cJSON_IsString(command_item)) {
                    continue;
                }
                
                targets[target_count].target_node_id = target_id;
                snprintf(targets[target_count].relay_command,
                        sizeof(targets[target_count].relay_command),
                        "%s", command_item->valuestring);
                
                target_count++;
                if (target_count >= num_targets) break;
            }
            
            // Store in connection map
            g_connections[dev_idx].buttons[button_idx].targets = targets;
            g_connections[dev_idx].buttons[button_idx].num_targets = target_count;
            
            ESP_LOGI(TAG, "Device %" PRIu32 " button '%c' → %d targets",
                     device_id, button_char, target_count);
        }
    }
    
    xSemaphoreGive(g_connections_mutex);
    cJSON_Delete(root);
    
    ESP_LOGI(TAG, "Connections parsed successfully");
}

// ====================
// Button Types Parsing (#14)
// ====================

void root_parse_button_types(const char *json_str)
{
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Button types JSON is NULL");
        return;
    }
    
    ESP_LOGI(TAG, "Parsing button types configuration");
    
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse button types JSON");
        return;
    }
    
    if (xSemaphoreTake(g_button_types_mutex, pdMS_TO_TICKS(ROUTING_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire button types mutex");
        cJSON_Delete(root);
        return;
    }
    
    // Clear existing button types
    memset(g_button_types, 0, sizeof(g_button_types));
    
    // Parse JSON: { "deviceId": { "button": type, ... }, ... }
    cJSON *device_item = NULL;
    cJSON_ArrayForEach(device_item, root) {
        const char *device_id_str = device_item->string;
        uint32_t device_id = parse_device_id_from_string(device_id_str);
        
        int dev_idx = root_find_device_index(device_id);
        if (dev_idx < 0) {
            continue;
        }
        
        // Iterate through buttons for this device
        cJSON *button_item = NULL;
        cJSON_ArrayForEach(button_item, device_item) {
            const char *button_str = button_item->string;
            if (strlen(button_str) != 1) continue;
            
            char button_char = button_str[0];
            int button_idx = button_char_to_index(button_char);
            
            if (button_idx < 0 || button_idx >= 16) continue;
            
            if (cJSON_IsNumber(button_item)) {
                uint8_t type = (uint8_t)button_item->valueint;
                g_button_types[dev_idx][button_idx] = type;
                
                ESP_LOGI(TAG, "Device %" PRIu32 " button '%c' type=%d",
                         device_id, button_char, type);
            }
        }
    }
    
    xSemaphoreGive(g_button_types_mutex);
    cJSON_Delete(root);
    
    ESP_LOGI(TAG, "Button types parsed successfully");
}

// ====================
// Button → Relay Routing (#15)
// ====================

void root_route_button_press(uint32_t from_device, char button, int state)
{
    ESP_LOGI(TAG, "Routing button '%c' from device %" PRIu32 " (state=%d)",
             button, from_device, state);
    
    // Convert button char to index
    int button_idx = button_char_to_index(button);
    
    if (button_idx < 0 || button_idx >= 16) {
        ESP_LOGW(TAG, "Invalid button character: %c", button);
        return;
    }
    
    // Find device index
    int dev_idx = -1;
    for (uint8_t i = 0; i < g_num_devices; i++) {
        if (g_device_ids[i] == from_device) {
            dev_idx = i;
            break;
        }
    }
    
    if (dev_idx < 0) {
        ESP_LOGD(TAG, "Device %" PRIu32 " not in routing table", from_device);
        return;
    }
    
    // Get button type
    uint8_t button_type = 0;
    if (xSemaphoreTake(g_button_types_mutex, pdMS_TO_TICKS(ROUTING_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        button_type = g_button_types[dev_idx][button_idx];
        xSemaphoreGive(g_button_types_mutex);
    }
    
    // Get routing targets
    if (xSemaphoreTake(g_connections_mutex, pdMS_TO_TICKS(ROUTING_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire connections mutex");
        return;
    }
    
    button_route_t *route = &g_connections[dev_idx].buttons[button_idx];
    
    if (route->num_targets == 0 || route->targets == NULL) {
        ESP_LOGD(TAG, "No routing targets for device %" PRIu32 " button '%c'",
                 from_device, button);
        xSemaphoreGive(g_connections_mutex);
        return;
    }
    
    ESP_LOGI(TAG, "Found %d routing targets", route->num_targets);
    
    // Route to each target
    for (uint8_t i = 0; i < route->num_targets; i++) {
        uint32_t target_node = route->targets[i].target_node_id;
        const char *base_cmd = route->targets[i].relay_command;
        
        // Build command based on button type
        char command[MAX_RELAY_COMMAND_LEN];
        if (button_type == 1 && state >= 0) {
            // Stateful button: append state (0 or 1)
            // Normalize state: 0 stays 0, any positive value becomes 1
            int button_state = (state > 0) ? 1 : 0;
            // Manually construct string to avoid format-truncation warning
            size_t base_len = strlen(base_cmd);
            // Check if base_cmd + digit + null terminator fits
            // (base_len + 1 < sizeof means we have room for base_len + 1 + 1 = base_len + 2)
            if (base_len + 1 < sizeof(command)) {
                // Copy base command (guaranteed to fit with null terminator)
                memcpy(command, base_cmd, base_len);
                command[base_len] = '0' + button_state;
                command[base_len + 1] = '\0';
            } else {
                // Base command too long, truncate it to fit with digit
                memcpy(command, base_cmd, sizeof(command) - 2);
                command[sizeof(command) - 2] = '0' + button_state;
                command[sizeof(command) - 1] = '\0';
            }
        } else {
            // Toggle button: use base command as-is
            strncpy(command, base_cmd, sizeof(command) - 1);
            command[sizeof(command) - 1] = '\0';
        }
        
        ESP_LOGI(TAG, "  Routing to node %" PRIu32 ": %s", target_node, command);
        
        // Send command via mesh
        mesh_app_msg_t msg = {0};
        msg.msg_type = MSG_TYPE_COMMAND;
        msg.device_id = g_device_id;  // From root
        
        size_t cmd_len = strlen(command);
        if (cmd_len >= sizeof(msg.data)) {
            cmd_len = sizeof(msg.data) - 1;
        }
        memcpy(msg.data, command, cmd_len);
        msg.data[cmd_len] = '\0';
        // NOTE: data_len includes null terminator for convenience in receivers
        // Receivers can treat data as null-terminated string without additional processing
        msg.data_len = cmd_len + 1;
        
        // Prepare mesh_data_t for sending
        mesh_data_t data;
        data.data = (uint8_t *)&msg;
        data.size = sizeof(mesh_app_msg_t);
        data.proto = MESH_PROTO_BIN;
        data.tos = MESH_TOS_P2P;
        
        // Broadcast to all nodes (target will filter by checking its own device ID)
        // LIMITATION: We still broadcast as we don't have MAC address mapping
        esp_mesh_send(NULL, &data, MESH_DATA_P2P, NULL, 0);
        
        if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(STATS_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            g_stats.button_presses++;
            xSemaphoreGive(g_stats_mutex);
        }
    }
    
    xSemaphoreGive(g_connections_mutex);
}
