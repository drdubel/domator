#include <inttypes.h>
#include <string.h>

#include "cJSON.h"
#include "domator_mesh.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "mqtt_client.h"

static const char* TAG = "ROOT";

// Static buffers for MQTT configuration (must persist after mqtt_init returns)
static char g_mqtt_client_id[32] = {0};
static char g_mqtt_lwt_message[256] = {0};

// Node registry - maps device_id to mesh address
#define MAX_NODES 64
typedef struct {
    uint32_t device_id;
    mesh_addr_t mesh_addr;
    char node_type[8];
    int64_t last_seen;
    int64_t last_ping;
    int32_t avg_ping;
    int outputs;
} node_registry_entry_t;

static node_registry_entry_t node_registry[MAX_NODES];
static int node_count = 0;
static SemaphoreHandle_t registry_mutex = NULL;

// Forward declarations
static void route_button_to_relays(uint32_t from_id, char button, int state);
static void handle_mqtt_command(const char* topic, int topic_len,
                                const char* data, int data_len);

// ============ NODE REGISTRY ============
static void registry_update(uint32_t device_id, mesh_addr_t* addr,
                            const char* type) {
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    for (int i = 0; i < node_count; i++) {
        if (node_registry[i].device_id == device_id) {
            memcpy(&node_registry[i].mesh_addr, addr, sizeof(mesh_addr_t));
            node_registry[i].last_seen = esp_timer_get_time() / 1000;
            if (type) strncpy(node_registry[i].node_type, type, 7);
            xSemaphoreGive(registry_mutex);
            return;
        }
    }
    if (node_count < MAX_NODES) {
        node_registry[node_count].device_id = device_id;
        memcpy(&node_registry[node_count].mesh_addr, addr, sizeof(mesh_addr_t));
        node_registry[node_count].last_seen = esp_timer_get_time() / 1000;
        if (type) strncpy(node_registry[node_count].node_type, type, 7);
        node_count++;
    }
    xSemaphoreGive(registry_mutex);
}

static mesh_addr_t* registry_find(uint32_t device_id) {
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (node_registry[i].device_id == device_id) {
            xSemaphoreGive(registry_mutex);
            return &node_registry[i].mesh_addr;
        }
    }
    xSemaphoreGive(registry_mutex);
    return NULL;
}

// Find device index by device ID
static int registry_find_index(uint32_t device_id) {
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (node_registry[i].device_id == device_id) {
            xSemaphoreGive(registry_mutex);
            return i;
        }
    }
    xSemaphoreGive(registry_mutex);
    return -1;
}

static int get_button_type(uint32_t device_id, char button) {
    xSemaphoreTake(g_button_types_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_connections[i].device_id == device_id) {
            int button_idx = button - 'a';
            if (button_idx >= 0 && button_idx < MAX_BUTTONS) {
                int type = g_button_types[i].types[button_idx];
                xSemaphoreGive(g_button_types_mutex);
                return type;
            }
        }
    }
    xSemaphoreGive(g_button_types_mutex);
    return -1;  // Not found
}

// ============ HANDLE MESH MESSAGES AS ROOT ============
void root_handle_mesh_message(mesh_addr_t* from, mesh_app_msg_t* msg) {
    registry_update(msg->src_id, from, NULL);
    ESP_LOGV(TAG, "Message from %" PRIu32 " (type=%c, len=%d)", msg->src_id,
             msg->msg_type, msg->data_len);

    switch (msg->msg_type) {
        case MSG_TYPE_BUTTON: {
            char button = msg->data[0];
            int state = (msg->data_len > 1) ? msg->data[1] - '0' : -1;

            ESP_LOGI(TAG, "Button '%c' from switch %" PRIu32, button,
                     msg->src_id);

            if (g_mqtt_connected) {
                char topic[64];
                snprintf(topic, sizeof(topic), "/switch/state/%" PRIu32,
                         msg->src_id);
                char payload[2] = {button, '\0'};
                ESP_LOGI(TAG, "Publishing button status to MQTT: %s", payload);
                esp_mqtt_client_publish(g_mqtt_client, topic, payload, 1, 0, 0);
            }

            route_button_to_relays(msg->src_id, button, state);
            break;
        }

        case MSG_TYPE_RELAY_STATE: {
            char relay_char = msg->data[0];
            char state_char = msg->data[1];
            ESP_LOGI(TAG, "Relay state '%c'='%c' from device %" PRIu32,
                     relay_char, state_char, msg->src_id);

            if (g_mqtt_connected) {
                char topic[64];
                snprintf(topic, sizeof(topic), "/relay/state/%" PRIu32,
                         msg->src_id);
                char payload[3] = {relay_char, state_char, '\0'};
                ESP_LOGI(TAG, "Publishing relay state to MQTT: %s", payload);
                esp_mqtt_client_publish(g_mqtt_client, topic, payload, 2, 0, 0);
            }
            break;
        }

        case MSG_TYPE_STATUS: {
            if (g_mqtt_connected) {
                char topic[64];
                snprintf(topic, sizeof(topic), "/switch/state/root");
                ESP_LOGI(TAG, "Publishing relay status to MQTT: %s", msg->data);
                esp_mqtt_client_publish(g_mqtt_client, topic, msg->data,
                                        msg->data_len, 0, 0);
            }
            break;
        }

        case MSG_TYPE_PING: {
            ESP_LOGV(TAG, "Received ping from %" PRIu32, msg->src_id);
            int index = registry_find_index(msg->src_id);

            uint16_t pingNum;
            memcpy(&pingNum, msg->data, sizeof(uint16_t));
            pingNum++;

            int now = esp_timer_get_time() / 1000;
            node_registry[index].avg_ping +=
                now - node_registry[index].last_ping;
            node_registry[index].last_ping = now;

            if (pingNum > PING_PONG_NUMBER) {
                node_registry[index].avg_ping /= pingNum;

                ESP_LOGW(TAG,
                         "Ping Pong communication test completed successfully "
                         "with device %" PRIu32,
                         msg->src_id);
                ESP_LOGW(TAG, "Average ping time: %" PRId32 " ms",
                         node_registry[index].avg_ping);
                break;
            }

            mesh_app_msg_t pong = {0};
            pong.src_id = g_device_id;
            pong.msg_type = MSG_TYPE_PING;
            pong.data_len = sizeof(uint16_t);
            memcpy(pong.data, &pingNum, sizeof(uint16_t));
            mesh_queue_to_node(from, &pong);
            ESP_LOGV(TAG, "Sent pong to %" PRIu32, msg->src_id);
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown msg type from %" PRIu32 ": %c", msg->src_id,
                     msg->msg_type);
            break;
    }
}

// ============ ROUTE BUTTON → RELAY (stub for now) ============
static void route_button_to_relays(uint32_t from_id, char button, int state) {
    // TODO: Port your connections map routing logic here
    // For now, just log
    ESP_LOGI(TAG, "Route button '%c' from %" PRIu32 " (state=%d)", button,
             from_id, state);

    int button_idx = button - 'a';
    if (button_idx < 0 || button_idx >= MAX_BUTTONS_EXTENDED) {
        ESP_LOGW(TAG, "Invalid button index: %d", button_idx);
        return;
    }
    button_route_t* route = NULL;
    xSemaphoreTake(g_connections_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_connections[i].device_id == from_id) {
            ESP_LOGI(TAG, "Found device index %d for device ID %" PRIu32, i,
                     from_id);
            route = &g_connections[i].buttons[button_idx];
            break;
        }
    }
    xSemaphoreGive(g_connections_mutex);

    if (route == NULL) {
        ESP_LOGI(TAG,
                 "No routing configured for button '%c' from device %" PRIu32,
                 button, from_id);
        return;
    }

    for (int j = 0; j < route->num_targets; j++) {
        route_target_t* target = &route->targets[j];
        mesh_addr_t* dest = registry_find(target->target_node_id);
        if (dest) {
            mesh_app_msg_t cmd = {0};
            cmd.src_id = g_device_id;
            cmd.msg_type = MSG_TYPE_COMMAND;
            cmd.data[0] = target->relay_command[0];
            if (get_button_type(from_id, button) == 1) {  // Stateful button
                cmd.data[1] = state + '0';
                cmd.data_len = 2;
            } else {
                cmd.data_len = 1;
            }
            mesh_queue_to_node(dest, &cmd);
            ESP_LOGI(TAG,
                     "Routed button '%c' of type %d from %" PRIu32
                     " to relay command '%c' on device %" PRIu32,
                     button, get_button_type(from_id, button), from_id,
                     target->relay_command[0], target->target_node_id);
        } else {
            ESP_LOGW(TAG, "No mesh address found for target device %" PRIu32,
                     target->target_node_id);
        }
    }
}

void root_publish_status(void) {
    if (!g_mqtt_client || !g_mqtt_connected || !g_is_root) {
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
        if (xSemaphoreTake(g_stats_mutex,
                           pdMS_TO_TICKS(STATS_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            g_stats.low_heap_events++;
            xSemaphoreGive(g_stats_mutex);
        }
    }

    // Get peer count (number of connected children)
    int peer_count = esp_mesh_get_total_node_num() - 1;  // Subtract self

    int8_t rssi = 0;
    wifi_ap_record_t ap_info;

    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    } else {
        ESP_LOGW(TAG, "Failed to get AP info for RSSI");
    }

    // Add type based on node type
    const char* type_str = "unknown";
    if (g_node_type == NODE_TYPE_SWITCH_C3) {
        type_str = "switch";
    } else if (g_node_type == NODE_TYPE_RELAY_8) {
        type_str = "relay8";
    } else if (g_node_type == NODE_TYPE_RELAY_16) {
        type_str = "relay16";
    }

    // Build JSON status report
    cJSON_AddNumberToObject(json, "deviceId", g_device_id);
    cJSON_AddNumberToObject(json, "parentId",
                            g_device_id);  // Root's parent is itself
    cJSON_AddStringToObject(json, "type", type_str);
    cJSON_AddNumberToObject(json, "isRoot", 1);
    cJSON_AddNumberToObject(json, "freeHeap", free_heap);
    cJSON_AddNumberToObject(json, "uptime", uptime);
    cJSON_AddNumberToObject(json, "meshLayer", g_mesh_layer);
    cJSON_AddNumberToObject(json, "peerCount", peer_count);
    cJSON_AddStringToObject(json, "firmware", g_firmware_hash);
    cJSON_AddNumberToObject(json, "rssi", rssi);
    cJSON_AddNumberToObject(json, "clicks", g_stats.button_presses);
    cJSON_AddNumberToObject(json, "lowHeap", g_stats.low_heap_events);

    char* json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (json_str != NULL) {
        char topic[64];
        snprintf(topic, sizeof(topic), "/switch/state/root");

        int msg_id =
            esp_mqtt_client_publish(g_mqtt_client, topic, json_str, 0, 0, 0);
        if (msg_id >= 0) {
            ESP_LOGI(TAG, "Published root status to %s: %s", topic, json_str);
        } else {
            ESP_LOGW(TAG, "Failed to publish root status to %s", topic);

            if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_stats.mqtt_dropped++;
                xSemaphoreGive(g_stats_mutex);
            }
        }

        free(json_str);
    }
}

// ==================== MQTT Connection Status ====================

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

    cJSON_AddStringToObject(json, "status",
                            connected ? "connected" : "disconnected");
    cJSON_AddNumberToObject(json, "device_id", g_device_id);
    cJSON_AddNumberToObject(json, "timestamp",
                            (double)(esp_timer_get_time() / 1000000));

    if (connected) {
        // Add additional info on connection
        cJSON_AddStringToObject(json, "firmware", g_firmware_hash);
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
        int msg_id = esp_mqtt_client_publish(
            g_mqtt_client, "/switch/state/root", json_str, 0, 1, 1);
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

// ============ MQTT EVENT HANDLER ============
static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                               int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t event = event_data;
    static bool connection_status_published = false;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            g_mqtt_connected = true;
            esp_mqtt_client_subscribe(g_mqtt_client, "/switch/cmd/+", 0);
            esp_mqtt_client_subscribe(g_mqtt_client, "/switch/cmd", 0);
            esp_mqtt_client_subscribe(g_mqtt_client, "/relay/cmd/+", 0);
            esp_mqtt_client_subscribe(g_mqtt_client, "/relay/cmd", 0);

            // Publish connection status only once per connection
            // (prevent duplicate publishes if event fires multiple times)
            if (!connection_status_published) {
                publish_connection_status(true);
                connection_status_published = true;
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            g_mqtt_connected = false;
            connection_status_published = false;  // Reset for next connection
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

// ============ HANDLE NON-JSON MQTT COMMANDS ============
static void handle_nonJson_mqtt_root_command(const char* topic, int topic_len,
                                             const char* data, int data_len) {
    if (data_len == 1 && data[0] == MSG_TYPE_PING) {
        // Send ping to all nodes in registry

        for (int i = 0; i < node_count; i++) {
            mesh_app_msg_t ping = {0};
            ping.src_id = g_device_id;
            ping.msg_type = MSG_TYPE_PING;
            uint16_t pingNum = 1;
            memcpy(ping.data, &pingNum, sizeof(uint16_t));
            ping.data_len = sizeof(uint16_t);

            int index = registry_find_index(node_registry[i].device_id);
            node_registry[index].last_ping = esp_timer_get_time() / 1000;
            mesh_queue_to_node(&node_registry[i].mesh_addr, &ping);
            ESP_LOGV(TAG, "Sent MQTT ping to device %" PRIu32,
                     node_registry[i].device_id);
        }
    }
}

// =========== HANDLE JSON MQTT COMMANDS ============
static void parse_json_connections(cJSON* data) {
    int device_index = 0;
    cJSON* device_item = NULL;
    xSemaphoreTake(g_connections_mutex, pdMS_TO_TICKS(100));

    cJSON_ArrayForEach(device_item, data) {
        if (device_index >= MAX_DEVICES) break;

        device_connections_t* device = &g_connections[device_index];
        memset(device, 0, sizeof(device_connections_t));

        // Set device_id from the JSON key
        device->device_id = (uint32_t)strtoul(device_item->string, NULL, 10);

        cJSON* button_map = device_item;  // object with keys "a"-"x"
        cJSON* button_entry = NULL;
        cJSON_ArrayForEach(button_entry, button_map) {
            const char* button_name = button_entry->string;
            int button_idx = button_name[0] - 'a';
            if (button_idx < 0 || button_idx >= MAX_BUTTONS_EXTENDED) continue;

            cJSON* targets_array =
                button_entry;  // array of [number, string] arrays
            int num_targets = cJSON_GetArraySize(targets_array);
            if (num_targets > 0) {
                route_target_t* targets =
                    malloc(sizeof(route_target_t) * num_targets);
                int t = 0;
                cJSON* inner_array = NULL;
                cJSON_ArrayForEach(inner_array, targets_array) {
                    if (!cJSON_IsArray(inner_array)) continue;
                    cJSON* node_id_item = cJSON_GetArrayItem(inner_array, 0);
                    cJSON* relay_item = cJSON_GetArrayItem(inner_array, 1);

                    if (cJSON_IsNumber(node_id_item) &&
                        cJSON_IsString(relay_item)) {
                        targets[t].target_node_id =
                            (uint32_t)node_id_item->valuedouble;
                        strncpy(targets[t].relay_command,
                                relay_item->valuestring,
                                sizeof(targets[t].relay_command) - 1);
                        targets[t]
                            .relay_command[sizeof(targets[t].relay_command) -
                                           1] = 0;
                        t++;
                    }
                }
                device->buttons[button_idx].targets = targets;
                device->buttons[button_idx].num_targets = t;
            }
        }

        device_index++;
    }
    xSemaphoreGive(g_connections_mutex);
}

static void parse_json_button_types(cJSON* data) {
    if (!data || !cJSON_IsObject(data)) return;

    int device_index = 0;
    cJSON* device_item = NULL;
    cJSON_ArrayForEach(device_item, data) {
        if (device_index >= MAX_DEVICES) break;

        button_types_t* device = &g_button_types[device_index];
        memset(device, 0, sizeof(button_types_t));

        // Set device_id from JSON key
        device->device_id = (uint32_t)strtoul(device_item->string, NULL, 10);

        cJSON* button_map = device_item;  // object with keys "a"-"h"
        cJSON* button_entry = NULL;
        cJSON_ArrayForEach(button_entry, button_map) {
            const char* button_name = button_entry->string;
            int button_idx = button_name[0] - 'a';  // 'a'-'h' -> 0-7
            if (button_idx < 0 || button_idx >= MAX_BUTTONS) continue;

            if (cJSON_IsNumber(button_entry)) {
                device->types[button_idx] = (uint8_t)button_entry->valueint;
            }
        }

        device_index++;
    }
}

static void handle_json_mqtt_root_command(const char* topic, int topic_len,
                                          const char* data, int data_len) {
    cJSON* json = cJSON_ParseWithLength(data, data_len);

    if (!json) {
        ESP_LOGE(TAG, "Failed to parse JSON command");
        return;
    }

    cJSON* msgType = cJSON_GetObjectItem(json, "type");
    if (!msgType || !cJSON_IsString(msgType)) {
        ESP_LOGE(TAG, "JSON command missing 'type' field");
        cJSON_Delete(json);
        return;
    }

    if (strcmp(msgType->valuestring, "connections") == 0) {
        cJSON* data = cJSON_GetObjectItem(json, "data");
        if (!data) {
            ESP_LOGE(TAG, "Connections command missing 'data' field");
            cJSON_Delete(json);
            return;
        }
        parse_json_connections(data);
    } else if (strcmp(msgType->valuestring, "button_types") == 0) {
        cJSON* data = cJSON_GetObjectItem(json, "data");
        if (!data) {
            ESP_LOGE(TAG, "Button types command missing 'data' field");
            cJSON_Delete(json);
            return;
        }
        parse_json_button_types(data);
    } else {
        ESP_LOGW(TAG, "Unknown JSON command type: %s", msgType->valuestring);
    }

    cJSON_Delete(json);
}

static void handle_nonJson_mqtt_command(const char* topic, int topic_len,
                                        const char* data, int data_len) {
    // For now, just log unrecognized non-JSON commands
    ESP_LOGW(TAG, "Received non-JSON MQTT command: %.*s -> %.*s", topic_len,
             topic, data_len, data);

    // Create null-terminated copy of topic for string operations
    char topic_str[128];
    int copy_len =
        (topic_len < sizeof(topic_str) - 1) ? topic_len : sizeof(topic_str) - 1;
    strncpy(topic_str, topic, copy_len);
    topic_str[copy_len] = '\0';

    char* last_slash = strrchr(topic_str, '/');
    if (last_slash && *(last_slash + 1) != '\0') {
        uint32_t target_id = (uint32_t)strtoul(last_slash + 1, NULL, 10);
        ESP_LOGI(TAG, "Non-JSON command for target device %" PRIu32, target_id);

        mesh_addr_t* dest = registry_find(target_id);
        if (dest) {
            mesh_app_msg_t cmd = {0};
            cmd.src_id = g_device_id;
            cmd.msg_type = MSG_TYPE_COMMAND;
            memcpy(cmd.data, data, data_len);
            cmd.data_len = data_len;
            mesh_queue_to_node(dest, &cmd);
            ESP_LOGI(TAG, "Routed non-JSON MQTT command to device %" PRIu32,
                     target_id);
        } else {
            ESP_LOGW(TAG, "Could not find target device %" PRIu32, target_id);
        }
    } else {
        ESP_LOGW(TAG, "MQTT topic does not contain target ID: %.*s", topic_len,
                 topic);
    }
}

// ============ HANDLE MQTT COMMANDS ============
static void handle_mqtt_command(const char* topic, int topic_len,
                                const char* data, int data_len) {
    // TODO: Port your mqttCallbackTask logic here
    // Parse topic, extract node ID, route command via mesh
    ESP_LOGI(TAG, "MQTT cmd: %.*s -> %.*s", topic_len, topic, data_len, data);

    char topic_str[128];
    int copy_len =
        (topic_len < sizeof(topic_str) - 1) ? topic_len : sizeof(topic_str) - 1;
    strncpy(topic_str, topic, copy_len);
    topic_str[copy_len] = '\0';

    if (strstr(topic_str, "/switch/cmd/root") != NULL) {
        ESP_LOGI(TAG, "Received root config command");
        // Handle root-specific config commands here

        if (data[0] != '{') {
            handle_nonJson_mqtt_root_command(topic, topic_len, data, data_len);
            return;
        } else {
            handle_json_mqtt_root_command(topic, topic_len, data, data_len);
            return;
        }
    } else {
        handle_nonJson_mqtt_command(topic, topic_len, data, data_len);
        return;
    }
}

// ============ ROOT START/STOP ============
void node_root_start(void) {
    if (g_mqtt_client) return;

    if (!registry_mutex) {
        registry_mutex = xSemaphoreCreateMutex();
    }

    ESP_LOGI(TAG, "Starting root services...");
}

// ====================
// MQTT Initialization
// ====================

void mqtt_init(void) {
    // Strong guard: Only root node should initialize MQTT
    if (!g_is_root) {
        ESP_LOGW(TAG,
                 "❌ MQTT init called on NON-ROOT node (device_id: %" PRIu32
                 ", layer: %d) - skipping",
                 g_device_id, g_mesh_layer);
        ESP_LOGW(TAG,
                 "   This is expected for leaf nodes. Only root connects "
                 "to MQTT.");
        return;
    }

    ESP_LOGI(TAG,
             "✓ Initializing MQTT client (ROOT node, device_id: %" PRIu32 ")",
             g_device_id);

    // Build complete MQTT broker URI
    char broker_uri[128];
    const char* url = CONFIG_MQTT_BROKER_URI;

    // Check if URL already includes port
    if (strstr(url, "mqtt://") != NULL && strchr(url + 7, ':') == NULL) {
        // No port in URL, append it
        snprintf(broker_uri, sizeof(broker_uri), "%s:%d", url,
                 1883);  // Default MQTT port
    } else {
        // URL already complete or has port
        snprintf(broker_uri, sizeof(broker_uri), "%s", url);
    }

    // Generate unique MQTT client ID based on device ID
    // Store in static buffer so it persists after function returns
    snprintf(g_mqtt_client_id, sizeof(g_mqtt_client_id), "domator_%" PRIu32,
             g_device_id);
    ESP_LOGI(TAG, "Using MQTT client ID: %s", g_mqtt_client_id);

    // Prepare Last Will and Testament (LWT) message for ungraceful
    // disconnects Store in static buffer so it persists after function
    // returns
    snprintf(g_mqtt_lwt_message, sizeof(g_mqtt_lwt_message),
             "{\"status\":\"disconnected\",\"device_id\":%" PRIu32
             ",\"timestamp\":%" PRId64 ",\"reason\":\"ungraceful\"}",
             g_device_id, esp_timer_get_time() / 1000000);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.client_id = g_mqtt_client_id,
        .credentials.username = CONFIG_MQTT_USER,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
        .session.last_will =
            {
                .topic = "/switch/state/root",
                .msg = g_mqtt_lwt_message,
                .msg_len = strlen(g_mqtt_lwt_message),
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
// MQTT Cleanup (when losing root status)
// ====================

void node_root_stop(void) {
    if (g_mqtt_client) {
        ESP_LOGI(TAG, "Cleaning up MQTT client (no longer root)");

        // Publish disconnection status if still connected
        if (g_mqtt_connected) {
            publish_connection_status(false);
        }

        // Stop and destroy MQTT client
        esp_mqtt_client_stop(g_mqtt_client);
        esp_mqtt_client_destroy(g_mqtt_client);
        g_mqtt_client = NULL;
        g_mqtt_connected = false;

        ESP_LOGI(TAG, "MQTT client cleaned up");
    }

    g_is_root = false;  // Ensure we update root status
    ESP_LOGI(TAG, "Root services stopped");
}