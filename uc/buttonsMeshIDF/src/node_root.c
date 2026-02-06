#include "domator_mesh.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "NODE_ROOT";

// ====================
// MQTT Command Handling
// ====================

// Helper: Parse device ID from string
static uint32_t parse_device_id_from_string(const char *str)
{
    if (str == NULL) return 0;
    return (uint32_t)strtoul(str, NULL, 10);
}

// Helper: Convert button character to index (0-15)
static int button_char_to_index(char button_char)
{
    if (button_char >= 'a' && button_char <= 'p') {
        return button_char - 'a';
    } else if (button_char >= 'A' && button_char <= 'P') {
        return button_char - 'A';
    }
    return -1;
}

static void handle_mqtt_command(const char *topic, int topic_len, 
                               const char *data, int data_len)
{
    if (topic == NULL || data == NULL || topic_len == 0 || data_len == 0) {
        return;
    }
    
    // Extract topic string
    char topic_str[128];
    int copy_len = (topic_len < sizeof(topic_str) - 1) ? topic_len : sizeof(topic_str) - 1;
    memcpy(topic_str, topic, copy_len);
    topic_str[copy_len] = '\0';
    
    // Extract command string (may be large for config)
    char *cmd_str = (char *)malloc(data_len + 1);
    if (cmd_str == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for command");
        return;
    }
    memcpy(cmd_str, data, data_len);
    cmd_str[data_len] = '\0';
    
    ESP_LOGI(TAG, "Processing MQTT command: topic=%s", topic_str);
    
    // Check if this is a config message to /switch/cmd/root (#19)
    if (strcmp(topic_str, "/switch/cmd/root") == 0) {
        // Try to parse as JSON config
        cJSON *json = cJSON_Parse(cmd_str);
        if (json != NULL) {
            ESP_LOGI(TAG, "Received config JSON");
            
            cJSON *type_item = cJSON_GetObjectItem(json, "type");
            cJSON *data_item = cJSON_GetObjectItem(json, "data");
            
            if (type_item != NULL && cJSON_IsString(type_item) &&
                data_item != NULL && cJSON_IsObject(data_item)) {
                
                const char *config_type = type_item->valuestring;
                char *data_str = cJSON_PrintUnformatted(data_item);
                
                if (data_str != NULL) {
                    if (strcmp(config_type, "connections") == 0) {
                        ESP_LOGI(TAG, "Parsing connections configuration");
                        root_parse_connections(data_str);
                    } else if (strcmp(config_type, "button_types") == 0) {
                        ESP_LOGI(TAG, "Parsing button types configuration");
                        root_parse_button_types(data_str);
                    } else {
                        ESP_LOGW(TAG, "Unknown config type: %s", config_type);
                    }
                    
                    free(data_str);
                }
            }
            
            cJSON_Delete(json);
            free(cmd_str);
            return;
        }
    }
    
    // Determine if this is a relay or switch command
    bool is_relay_cmd = (strstr(topic_str, "/relay/cmd") != NULL);
    bool is_switch_cmd = (strstr(topic_str, "/switch/cmd") != NULL);
    
    if (!is_relay_cmd && !is_switch_cmd) {
        ESP_LOGW(TAG, "Unknown command topic: %s", topic_str);
        free(cmd_str);
        return;
    }
    
    // Extract device ID if present (e.g., /relay/cmd/12345678)
    uint32_t target_device_id = 0;
    char *last_slash = strrchr(topic_str, '/');
    if (last_slash != NULL && last_slash[1] != '\0') {
        // Try to parse as device ID
        char *endptr;
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
    mesh_data_t data;
    data.data = (uint8_t *)&msg;
    data.size = sizeof(mesh_app_msg_t);
    data.proto = MESH_PROTO_BIN;
    data.tos = MESH_TOS_P2P;
    
    // LIMITATION: Direct device addressing not yet implemented
    // ESP-WIFI-MESH requires MAC address for direct routing, but we only have device ID
    // Current workaround: All commands are broadcast to all nodes
    // Each node checks if command is relevant and ignores if not
    // Future: Maintain a device ID -> MAC address mapping table for direct routing
    
    if (target_device_id != 0) {
        ESP_LOGW(TAG, "Specific device targeting (%" PRIu32 ") not implemented, broadcasting", 
                 target_device_id);
        ESP_LOGI(TAG, "Broadcasting command to all nodes (intended for %" PRIu32 ")", 
                 target_device_id);
        esp_mesh_send(NULL, &data, MESH_DATA_P2P, NULL, 0);
    } else {
        // Broadcast to all nodes
        ESP_LOGI(TAG, "Broadcasting command to all nodes");
        esp_mesh_send(NULL, &data, MESH_DATA_P2P, NULL, 0);
    }
    
    free(cmd_str);
}

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
            esp_mqtt_client_subscribe(g_mqtt_client, "/switch/cmd/root", 0);  // For config
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
            handle_mqtt_command(event->topic, event->topic_len,
                              event->data, event->data_len);
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
        if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(STATS_MUTEX_TIMEOUT_MS)) == pdTRUE) {
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
            
            if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(STATS_MUTEX_TIMEOUT_MS)) == pdTRUE) {
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
            
            if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(STATS_MUTEX_TIMEOUT_MS)) == pdTRUE) {
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
                int button_state = -1;  // Default: toggle (-1), or 0/1 for stateful
                
                // Check if this is a stateful button press (data_len >= 2)
                if (msg->data_len >= 2 && (msg->data[1] == '0' || msg->data[1] == '1')) {
                    button_state = msg->data[1] - '0';
                }
                
                ESP_LOGI(TAG, "Button '%c' pressed on device %" PRIu32 " (state=%d)",
                         button_char, msg->device_id, button_state);
                
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
                        
                        if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(STATS_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                            g_stats.mqtt_dropped++;
                            xSemaphoreGive(g_stats_mutex);
                        }
                    }
                }
                
                // Route button press to configured relay targets (#15)
                root_route_button_press(msg->device_id, button_char, button_state);
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
                // Format: state_str should be like "a0" (relay a is OFF) or "a1" (relay a is ON)
                if (g_mqtt_connected && strlen(state_str) >= 2) {
                    char topic[64];
                    char relay_char = state_str[0];
                    char state_char = state_str[1];
                    
                    snprintf(topic, sizeof(topic), 
                             "/relay/state/%" PRIu32 "/%c", 
                             msg->device_id, relay_char);
                    
                    char payload[2] = {state_char, '\0'};
                    
                    int mqtt_msg_id = esp_mqtt_client_publish(g_mqtt_client, topic,
                                                             payload, 1, 0, 0);
                    if (mqtt_msg_id >= 0) {
                        ESP_LOGD(TAG, "Published relay state to %s: %c", topic, state_char);
                    } else {
                        ESP_LOGW(TAG, "Failed to publish relay state");
                        
                        if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(STATS_MUTEX_TIMEOUT_MS)) == pdTRUE) {
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
                strncpy(targets[target_count].relay_command, command_item->valuestring, 
                        sizeof(targets[target_count].relay_command) - 1);
                targets[target_count].relay_command[sizeof(targets[target_count].relay_command) - 1] = '\0';
                
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
            snprintf(command, sizeof(command), "%s%d", base_cmd, state);
        } else {
            // Toggle button: use base command
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
