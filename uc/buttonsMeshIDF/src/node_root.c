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
    for (int i = 0; i < node_count; i++) {
        if (node_registry[i].device_id == device_id) {
            return &node_registry[i].mesh_addr;
        }
    }
    return NULL;
}

// ============ HANDLE MESH MESSAGES AS ROOT ============
void root_handle_mesh_message(mesh_addr_t* from, mesh_app_msg_t* msg) {
    registry_update(msg->src_id, from, NULL);
    ESP_LOGI(TAG, "Message from %" PRIu32 " (type=%c, len=%d)", msg->src_id,
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
                esp_mqtt_client_publish(g_mqtt_client, topic, payload, 0, 0, 0);
            }

            route_button_to_relays(msg->src_id, button, state);
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

    // Example of how it will work:
    // mesh_addr_t *dest = registry_find(target_relay_id);
    // if (dest) {
    //     mesh_app_msg_t cmd = { .src_id = g_device_id, .msg_type = 'C' };
    //     cmd.data[0] = output_letter;
    //     cmd.data[1] = state + '0';
    //     cmd.data_len = 2;
    //     mesh_queue_to_node(dest, &cmd);
    // }
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

    cJSON_AddStringToObject(json, "status",
                            connected ? "connected" : "disconnected");
    cJSON_AddNumberToObject(json, "device_id", g_device_id);
    cJSON_AddNumberToObject(json, "timestamp",
                            (double)(esp_timer_get_time() / 1000000));

    if (connected) {
        // Add additional info on connection
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
        int msg_id = esp_mqtt_client_publish(
            g_mqtt_client, "/switch/state/root/", json_str, 0, 1, 1);
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

// ============ HANDLE MQTT COMMANDS (stub for now) ============
static void handle_mqtt_command(const char* topic, int topic_len,
                                const char* data, int data_len) {
    // TODO: Port your mqttCallbackTask logic here
    // Parse topic, extract node ID, route command via mesh
    ESP_LOGI(TAG, "MQTT cmd: %.*s -> %.*s", topic_len, topic, data_len, data);
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
        ESP_LOGW(
            TAG,
            "   This is expected for leaf nodes. Only root connects to MQTT.");
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

    // Prepare Last Will and Testament (LWT) message for ungraceful disconnects
    // Store in static buffer so it persists after function returns
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
                .topic = "/switch/state/root/",
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