/**
 * @file node_root.c
 * @brief Root-node logic: MQTT client, node registry, button routing, and
 *        MQTT command handling.
 *
 * A node becomes root dynamically via ESP-MESH self-organised election.
 * When elected, it:
 *  - Maintains a registry (node_registry) mapping device IDs to mesh addresses.
 *  - Connects to the MQTT broker and subscribes to command topics.
 *  - Publishes relay state, button state, and device status messages.
 *  - Routes button press events to relay nodes based on the connection map
 *    (g_connections) pushed via MQTT JSON commands.
 *  - Handles ping/pong round-trip latency tests.
 *
 * When the node loses root status, node_root_stop() tears down the MQTT client
 * and stops the Telnet server.
 */

#include <inttypes.h>
#include <string.h>

#include "cJSON.h"
#include "domator_mesh.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "mqtt_client.h"

static const char* TAG = "ROOT";
static const int MAX_MQTT_PAYLOAD_SIZE = 8192;

/** Persistent MQTT configuration buffers (must outlive mqtt_init). */
static char g_mqtt_client_id[32] = {0};
static char g_mqtt_lwt_message[256] = {0};

/** @brief Maps device_id to mesh address and last-seen metadata. */
typedef struct {
    uint64_t device_id;
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

static void route_button_to_relays(uint64_t from_id, char button, int state);
static void handle_mqtt_command(const char* topic, int topic_len,
                                const char* data, int data_len);

// ====================
// Node Registry
// ====================

/**
 * @brief Insert or update a node_registry entry for the given device.
 *        Thread-safe; acquires registry_mutex.  If device_id is not yet
 *        in the registry and there is space, a new slot is allocated.
 * @param device_id Unique numeric device identifier.
 * @param addr      Current mesh address of the device.
 * @param type      Optional one-character device type string; pass NULL to
 *                  leave the existing type unchanged.
 */
static void registry_update(uint64_t device_id, mesh_addr_t* addr,
                            const char* type) {
    if (xSemaphoreTake(registry_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "registry_update: mutex timeout");
        return;
    }
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

/**
 * @brief Look up the mesh address for a device by ID.
 * @param device_id Device to locate.
 * @return Pointer to the stored mesh_addr_t, or NULL if not found.
 */
static bool registry_find(uint64_t device_id, mesh_addr_t* out_addr) {
    if (out_addr == NULL) {
        return false;
    }

    if (xSemaphoreTake(registry_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "registry_find: mutex timeout");
        return false;
    }
    for (int i = 0; i < MAX_NODES; i++) {
        if (node_registry[i].device_id == device_id) {
            memcpy(out_addr, &node_registry[i].mesh_addr, sizeof(mesh_addr_t));
            xSemaphoreGive(registry_mutex);
            return true;
        }
    }
    xSemaphoreGive(registry_mutex);
    return false;
}

/**
 * @brief Retrieve the configured button type for a specific button on a device.
 * @param device_id Source device ID.
 * @param button    Button character ('a'-'h').
 * @return 0 = toggle, 1 = stateful, or -1 if not found.
 */
static int get_button_type(uint64_t device_id, char button) {
    if (xSemaphoreTake(g_button_types_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "get_button_type: mutex timeout");
        return -1;
    }
    for (int i = 0; i < MAX_NODES; i++) {
        if (g_button_types[i].device_id == device_id) {
            int button_idx = button - 'a';
            if (button_idx >= 0 && button_idx < MAX_BUTTONS) {
                int type = g_button_types[i].types[button_idx];
                xSemaphoreGive(g_button_types_mutex);
                return type;
            }
        }
    }
    xSemaphoreGive(g_button_types_mutex);
    return -1;
}

// ====================
// Handle Mesh Messages as Root
// ====================

/**
 * @brief Dispatch an incoming mesh message received while this node is root.
 * @param from Mesh address of the sender.
 * @param msg  Pointer to the decoded application message.
 */
void root_handle_mesh_message(mesh_addr_t* from, mesh_app_msg_t* msg) {
    registry_update(msg->src_id, from, NULL);
    ESP_LOGV(TAG, "Message from %" PRIu64 " (type=%c, len=%d)", msg->src_id,
             msg->msg_type, msg->data_len);

    switch (msg->msg_type) {
        case MSG_TYPE_COMMAND: {
            ESP_LOGI(TAG, "Command received: %.*s", msg->data_len, msg->data);

            if (g_node_type == NODE_TYPE_RELAY_8 ||
                g_node_type == NODE_TYPE_RELAY_16) {
                relay_handle_command((char*)msg->data);
            }

            break;
        }

        case MSG_TYPE_SYNC_REQUEST: {
            ESP_LOGI(TAG, "Received sync request from root");

            if (g_node_type == NODE_TYPE_RELAY_8 ||
                g_node_type == NODE_TYPE_RELAY_16) {
                relay_sync_all_states();
            }

            break;
        }

        case MSG_TYPE_BUTTON: {
            char button = msg->data[0];
            int state = (msg->data_len > 1) ? msg->data[1] - '0' : -1;

            ESP_LOGI(TAG, "Button '%c' from switch %" PRIu64, button,
                     msg->src_id);

            int button_type = get_button_type(msg->src_id, button);

            if (button_type == 0 && state == 1) {
                ESP_LOGI(TAG, "Toggle button '%c' pressed from device %" PRIu64,
                         button, msg->src_id);
                break;
            }

            if (g_mqtt_connected) {
                char topic[64];
                snprintf(topic, sizeof(topic), "/switch/state/%" PRIu64,
                         msg->src_id);

                if (button_type == 0) {
                    char payload[2] = {button, '\0'};
                    ESP_LOGI(TAG, "Publishing button status to MQTT: %s",
                             payload);
                    esp_mqtt_client_publish(g_mqtt_client, topic, payload, 2, 1,
                                            0);
                } else {
                    char payload[3] = {button, state + '0', '\0'};
                    ESP_LOGI(TAG, "Publishing button status to MQTT: %s",
                             payload);
                    esp_mqtt_client_publish(g_mqtt_client, topic, payload, 3, 1,
                                            0);
                }
            }

            route_button_to_relays(msg->src_id, button, state);
            break;
        }

        case MSG_TYPE_RELAY_STATE: {
            char relay_char = msg->data[0];
            char state_char = msg->data[1];
            ESP_LOGI(TAG, "Relay state '%c'='%c' from device %" PRIu64,
                     relay_char, state_char, msg->src_id);

            if (g_mqtt_connected) {
                char topic[64];
                snprintf(topic, sizeof(topic), "/relay/state/%" PRIu64,
                         msg->src_id);
                char payload[3] = {relay_char, state_char, '\0'};
                ESP_LOGI(TAG, "Publishing relay state to MQTT: %s", payload);
                esp_mqtt_client_publish(g_mqtt_client, topic, payload, 2, 1, 1);
            }
            break;
        }

        case MSG_TYPE_OTA_START: {
            ESP_LOGI(TAG, "OTA update requested by device %" PRIu64,
                     msg->src_id);
            g_ota_requested = true;
            break;
        }

        case MSG_TYPE_STATUS: {
            if (g_mqtt_connected) {
                char topic[64];
                snprintf(topic, sizeof(topic), "/switch/state/root");
                ESP_LOGV(TAG, "Publishing device status to MQTT: %s",
                         msg->data);
                esp_mqtt_client_publish(g_mqtt_client, topic, msg->data,
                                        msg->data_len, 0, 0);
            }
            break;
        }

        case MSG_TYPE_TYPE_INFO: {
            char type_str;
            memcpy(&type_str, msg->data, msg->data_len);
            ESP_LOGI(TAG, "Device type info from %" PRIu64 ": %c", msg->src_id,
                     type_str);
            registry_update(msg->src_id, from, &type_str);

            if (type_str == DEVICE_TYPE_RELAY) {
                mesh_app_msg_t sync_msg = {0};
                sync_msg.src_id = g_device_id;
                sync_msg.msg_type = MSG_TYPE_SYNC_REQUEST;
                mesh_queue_to_node(&sync_msg, TX_PRIO_NORMAL, from);
                ESP_LOGI(TAG, "Sent sync request to device %" PRIu64,
                         msg->src_id);
            }

            break;
        }

        case MSG_TYPE_PING: {
            ESP_LOGV(TAG, "Received ping from %" PRIu64, msg->src_id);
            if (xSemaphoreTake(registry_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
                ESP_LOGE(TAG, "MSG_TYPE_PING: registry mutex timeout");
                break;
            }

            int index = -1;
            for (int i = 0; i < MAX_NODES; i++) {
                if (node_registry[i].device_id == msg->src_id) {
                    index = i;
                    break;
                }
            }

            if (index < 0) {
                xSemaphoreGive(registry_mutex);
                ESP_LOGW(TAG, "Ping source not found in registry: %" PRIu64,
                         msg->src_id);
                break;
            }

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
                         "with device %" PRIu64 ". Average ping time: %" PRId32
                         " ms",
                         msg->src_id, node_registry[index].avg_ping);

                char topic[64];
                snprintf(topic, sizeof(topic), "/switch/state/%" PRIu64,
                         msg->src_id);
                char payload[32];
                snprintf(payload, sizeof(payload), "%" PRId32,
                         node_registry[index].avg_ping);
                esp_mqtt_client_publish(g_mqtt_client, topic, payload,
                                        strlen(payload), 0, 0);
                xSemaphoreGive(registry_mutex);
                break;
            }

            xSemaphoreGive(registry_mutex);

            mesh_app_msg_t pong = {0};
            pong.src_id = g_device_id;
            pong.msg_type = MSG_TYPE_PING;
            pong.data_len = sizeof(uint16_t);
            memcpy(pong.data, &pingNum, sizeof(uint16_t));
            mesh_queue_to_node(&pong, TX_PRIO_HIGH, from);
            ESP_LOGV(TAG, "Sent pong to %" PRIu64, msg->src_id);
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown msg type from %" PRIu64 ": %c", msg->src_id,
                     msg->msg_type);
            break;
    }
}

// ====================
// Route Button to Relays
// ====================

/**
 * @brief Forward a button event to all configured relay targets.
 *
 * Looks up the routing table (g_connections) for the source device and
 * button character, then sends a MSG_TYPE_COMMAND to each target relay node.
 * For stateful buttons the command includes the current state; for toggle
 * buttons a single-byte toggle command is sent.
 *
 * @param from_id Source device ID (the switch that was pressed).
 * @param button  Button character ('a' – 'x').
 * @param state   Physical button state: 1 = pressed, 0 = released.
 */
static void route_button_to_relays(uint64_t from_id, char button, int state) {
    ESP_LOGI(TAG, "Route button '%c' from %" PRIu64 " (state=%d)", button,
             from_id, state);

    int button_idx = button - 'a';
    if (button_idx < 0 || button_idx >= MAX_BUTTONS_EXTENDED) {
        ESP_LOGW(TAG, "Invalid button index: %d", button_idx);
        return;
    }
    button_route_t* route = NULL;
    if (xSemaphoreTake(g_connections_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "route_button_to_relays: mutex timeout");
        return;
    }
    for (int i = 0; i < MAX_NODES; i++) {
        if (g_connections[i].device_id == from_id) {
            ESP_LOGI(TAG, "Found device index %d for device ID %" PRIu64, i,
                     from_id);
            route = &g_connections[i].buttons[button_idx];
            break;
        }
    }
    xSemaphoreGive(g_connections_mutex);

    if (route == NULL) {
        ESP_LOGI(TAG,
                 "No routing configured for button '%c' from device %" PRIu64,
                 button, from_id);
        return;
    }

    for (int j = 0; j < route->num_targets; j++) {
        route_target_t* target = &route->targets[j];
        mesh_addr_t dest = {0};
        if (registry_find(target->target_node_id, &dest)) {
            mesh_app_msg_t cmd = {0};
            cmd.src_id = g_device_id;
            cmd.msg_type = MSG_TYPE_COMMAND;
            cmd.data[0] = target->relay_command[0];
            if (get_button_type(from_id, button) == 1) {  // Stateful button
                cmd.data[1] = state ? '0' : '1';          // '0' or '1'
                cmd.data_len = 2;
            } else {
                cmd.data_len = 1;
            }
            mesh_queue_to_node(&cmd, TX_PRIO_NORMAL, &dest);
            ESP_LOGI(TAG,
                     "Routed button '%c' of type %d from %" PRIu64
                     " to relay command '%c' on device %" PRIu64,
                     button, get_button_type(from_id, button), from_id,
                     target->relay_command[0], target->target_node_id);
        } else {
            ESP_LOGW(TAG, "No mesh address found for target device %" PRIu64,
                     target->target_node_id);
        }
    }
}

// ====================
// Root Status Publishing
// ====================

/**
 * @brief Build and publish a JSON status report for the root node to MQTT.
 */
void root_publish_status(void) {
    if (!g_mqtt_client || !g_mqtt_connected || !g_is_root) {
        return;
    }

    cJSON* json = cJSON_CreateObject();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return;
    }

    uint32_t uptime = esp_timer_get_time() / 1000000;

    uint32_t free_heap = esp_get_free_heap_size();

    if (free_heap < LOW_HEAP_THRESHOLD) {
        if (xSemaphoreTake(g_stats_mutex,
                           pdMS_TO_TICKS(STATS_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            g_stats.low_heap_events++;
            xSemaphoreGive(g_stats_mutex);
        }
    }

    int peer_count = esp_mesh_get_total_node_num() - 1;

    int8_t rssi = 0;
    wifi_ap_record_t ap_info;

    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    } else {
        ESP_LOGW(TAG, "Failed to get AP info for RSSI");
    }

    const char* type_str = "unknown";
    if (g_node_type == NODE_TYPE_SWITCH_C3) {
        type_str = "switch";
    } else if (g_node_type == NODE_TYPE_RELAY_8) {
        type_str = "relay8";
    } else if (g_node_type == NODE_TYPE_RELAY_16) {
        type_str = "relay16";
    }

    cJSON_AddNumberToObject(json, "deviceId", g_device_id);
    cJSON_AddNumberToObject(json, "parentId", g_device_id);
    cJSON_AddStringToObject(json, "type", type_str);
    cJSON_AddNumberToObject(json, "isRoot", 1);
    cJSON_AddNumberToObject(json, "freeHeap", free_heap);
    cJSON_AddNumberToObject(json, "uptime", uptime);
    cJSON_AddNumberToObject(json, "meshLayer", g_mesh_layer);
    cJSON_AddNumberToObject(json, "peerCount", peer_count);
    cJSON_AddNumberToObject(json, "firmware", g_firmware_timestamp);
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
            ESP_LOGV(TAG, "Published root status to %s: %s", topic, json_str);
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
 * @brief Publish a JSON connection status message to the root MQTT topic.
 *        Called on MQTT connect and on graceful disconnect.
 * @param connected true = publish connected payload, false = disconnected.
 */
static void publish_connection_status(bool connected) {
    if (!g_mqtt_client) {
        return;
    }

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
        cJSON_AddNumberToObject(json, "firmware", g_firmware_timestamp);
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

// ====================
// MQTT Event Handler
// ====================

/**
 * @brief Handle MQTT client lifecycle events.
 *
 * On MQTT_EVENT_CONNECTED subscribes to switch/relay command topics and
 * publishes a connection status message (once per connection).  On
 * MQTT_EVENT_DATA forwards the payload to handle_mqtt_command().
 */
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

            if (!connection_status_published) {
                publish_connection_status(true);
                connection_status_published = true;
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            g_mqtt_connected = false;
            connection_status_published = false;
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
// Handle Non-JSON MQTT Root Commands
// ====================

/**
 * @brief Handle non-JSON commands addressed to the root itself
 *        (e.g., a single-byte ping that broadcasts latency tests to all nodes).
 */
static void handle_nonJson_mqtt_root_command(const char* topic, int topic_len,
                                             const char* data, int data_len) {
    if (data_len == 1 && data[0] == MSG_TYPE_PING) {
        // Send ping to all nodes in registry
        mesh_addr_t targets[MAX_NODES] = {0};
        uint64_t target_ids[MAX_NODES] = {0};
        int target_indices[MAX_NODES] = {0};
        int64_t ping_timestamps[MAX_NODES] = {0};
        int target_count = 0;

        if (xSemaphoreTake(registry_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "handle_nonJson_mqtt_root_command: mutex timeout");
            return;
        }

        for (int i = 0; i < MAX_NODES; i++) {
            if (node_registry[i].device_id == 0 ||
                node_registry[i].device_id == g_device_id)
                continue;
            memcpy(&targets[target_count], &node_registry[i].mesh_addr,
                   sizeof(mesh_addr_t));
            target_ids[target_count] = node_registry[i].device_id;
            target_indices[target_count] = i;
            target_count++;
        }

        xSemaphoreGive(registry_mutex);

        for (int i = 0; i < target_count; i++) {
            mesh_app_msg_t ping = {0};
            ping.src_id = g_device_id;
            ping.msg_type = MSG_TYPE_PING;
            uint16_t pingNum = 1;
            memcpy(ping.data, &pingNum, sizeof(uint16_t));
            ping.data_len = sizeof(uint16_t);

            if (mesh_queue_to_node(&ping, TX_PRIO_HIGH, &targets[i])) {
                // Record the time we queued the ping; update last_ping later
                ping_timestamps[i] = esp_timer_get_time() / 1000;
                ESP_LOGV(TAG, "Sent MQTT ping to device %" PRIu64,
                         target_ids[i]);
            } else {
                ESP_LOGW(TAG, "Failed to enqueue ping for device %" PRIu64,
                         target_ids[i]);
            }
        }

        // Batch-update last_ping under a single mutex acquisition
        if (target_count > 0) {
            if (xSemaphoreTake(registry_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
                ESP_LOGE(TAG,
                         "handle_nonJson_mqtt_root_command: mutex timeout "
                         "updating last_ping");
                return;
            }

            for (int i = 0; i < target_count; i++) {
                if (ping_timestamps[i] == 0) {
                    continue;  // ping was not successfully queued for this
                               // target
                }
                int registry_idx = target_indices[i];
                if (registry_idx >= 0 && registry_idx < MAX_NODES &&
                    node_registry[registry_idx].device_id == target_ids[i]) {
                    node_registry[registry_idx].last_ping = ping_timestamps[i];
                }
            }

            xSemaphoreGive(registry_mutex);
        }
    }
}

// ====================
// Handle JSON MQTT Root Commands
// ====================

/**
 * @brief Parse a "connections" JSON payload and populate g_connections.
 *
 * Expected format:
 * @code
 * { "<device_id>": { "a": [[<node_id>, "<relay_cmd>"], ...], "b": ... } }
 * @endcode
 * Frees any previously allocated route_target_t arrays before replacing them.
 * @param data cJSON object containing the connection map.
 */
static void parse_json_connections(cJSON* data) {
    int device_index = 0;
    cJSON* device_item = NULL;
    xSemaphoreTake(g_connections_mutex, pdMS_TO_TICKS(100));

    cJSON_ArrayForEach(device_item, data) {
        if (device_index >= MAX_NODES) break;

        device_connections_t* device = &g_connections[device_index];
        for (int i = 0; i < MAX_BUTTONS_EXTENDED; i++) {
            if (device->buttons[i].targets != NULL) {
                free(device->buttons[i].targets);
            }
        }
        memset(device, 0, sizeof(device_connections_t));

        // Set device_id from the JSON key
        device->device_id = (uint64_t)strtoull(device_item->string, NULL, 10);

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
                            (uint64_t)node_id_item->valuedouble;
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

/**
 * @brief Log the full button type table for all known devices (debug helper).
 */
static void print_all_button_types(void) {
    if (xSemaphoreTake(g_button_types_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "print_all_button_types: mutex timeout");
        return;
    }
    ESP_LOGI(TAG, "Button types for all devices:");
    for (int i = 0; i < MAX_NODES; i++) {
        if (g_button_types[i].device_id == 0) continue;
        ESP_LOGI(TAG, "Device %" PRIu64 ":", g_button_types[i].device_id);
        for (int j = 0; j < MAX_BUTTONS; j++) {
            ESP_LOGI(TAG, "  Button %c: %d", 'a' + j,
                     g_button_types[i].types[j]);
        }
    }
    xSemaphoreGive(g_button_types_mutex);
}

/**
 * @brief Parse a "button_types" JSON payload and populate g_button_types.
 *
 * Expected format:
 * @code
 * { "<device_id>": { "a": 0, "b": 1, ... } }  // 0=toggle, 1=stateful
 * @endcode
 * @param data cJSON object containing per-device button type maps.
 */
static void parse_json_button_types(cJSON* data) {
    if (!data || !cJSON_IsObject(data)) return;

    int device_index = 0;
    cJSON* device_item = NULL;
    cJSON_ArrayForEach(device_item, data) {
        if (device_index >= MAX_NODES) break;

        button_types_t* device = &g_button_types[device_index];
        memset(device, 0, sizeof(button_types_t));

        // Set device_id from JSON key
        device->device_id = (uint64_t)strtoull(device_item->string, NULL, 10);

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

    print_all_button_types();
}

/**
 * @brief Parse and route per-relay auto-off timers.
 *
 * Expected format:
 * @code
 * { "<relay_id>": { "a": 120, "b": 0, ... }, ... }
 * @endcode
 * For each entry, routes a relay command in the form: T<output><seconds>
 */
static void parse_json_auto_off(cJSON* data) {
    if (!data || !cJSON_IsObject(data)) {
        return;
    }

    cJSON* relay_item = NULL;
    cJSON_ArrayForEach(relay_item, data) {
        if (!relay_item->string || !cJSON_IsObject(relay_item)) {
            continue;
        }

        uint64_t relay_id = strtoull(relay_item->string, NULL, 10);
        if (relay_id == 0) {
            continue;
        }

        mesh_addr_t dest = {0};
        if (!registry_find(relay_id, &dest)) {
            ESP_LOGW(TAG,
                     "Auto-off config: relay %" PRIu64 " not found in registry",
                     relay_id);
            continue;
        }

        cJSON* output_item = NULL;
        cJSON_ArrayForEach(output_item, relay_item) {
            if (!output_item->string || !cJSON_IsNumber(output_item)) {
                continue;
            }

            char output_char = output_item->string[0];
            if (output_char >= 'A' && output_char <= 'Z') {
                output_char = (char)(output_char - 'A' + 'a');
            }
            if (output_char < 'a' || output_char > 'z') {
                continue;
            }

            int timeout_seconds = output_item->valueint;
            if (timeout_seconds < 0) {
                timeout_seconds = 0;
            }

            mesh_app_msg_t cmd = {0};
            cmd.src_id = g_device_id;
            cmd.msg_type = MSG_TYPE_COMMAND;
            int written = snprintf(cmd.data, sizeof(cmd.data), "T%c%d",
                                   output_char, timeout_seconds);
            if (written <= 0 || written >= (int)sizeof(cmd.data)) {
                ESP_LOGW(TAG,
                         "Auto-off config command too long for relay %" PRIu64,
                         relay_id);
                continue;
            }
            cmd.data_len = written;

            mesh_queue_to_node(&cmd, TX_PRIO_NORMAL, &dest);
            ESP_LOGI(TAG, "Routed auto-off config to relay %" PRIu64 ": %s",
                     relay_id, cmd.data);
        }
    }
}

/**
 * @brief Dispatch a JSON MQTT command received on /switch/cmd/root.
 *
 * Supported types:
 *  - "connections"  – update the button-to-relay routing table.
 *  - "button_types" – update the toggle/stateful classification per button.
 *  - "auto_off"     – update per-relay output auto-off timeout values.
 */
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
    } else if (strcmp(msgType->valuestring, "auto_off") == 0) {
        cJSON* data = cJSON_GetObjectItem(json, "data");
        if (!data) {
            ESP_LOGE(TAG, "Auto-off command missing 'data' field");
            cJSON_Delete(json);
            return;
        }
        parse_json_auto_off(data);
    } else {
        ESP_LOGW(TAG, "Unknown JSON command type: %s", msgType->valuestring);
    }

    cJSON_Delete(json);
}

/**
 * @brief Handle non-JSON MQTT commands targeting device nodes.
 *
 * Extracts device type and optional target device ID from the topic, then
 * routes MSG_TYPE_COMMAND, MSG_TYPE_OTA_START, or MSG_TYPE_PING to the
 * appropriate mesh node (or broadcasts OTA to all nodes of the given type).
 */
static void handle_nonJson_mqtt_command(const char* topic, int topic_len,
                                        const char* data, int data_len) {
    // For now, just log unrecognized non-JSON commands
    ESP_LOGW(TAG, "Received non-JSON MQTT command");

    // Create null-terminated copy of topic for string operations
    char topic_str[128];
    int copy_len =
        (topic_len < sizeof(topic_str) - 1) ? topic_len : sizeof(topic_str) - 1;
    strncpy(topic_str, topic, copy_len);
    topic_str[copy_len] = '\0';

    char* first_slash = strchr(topic_str, '/');
    char* second_slash = NULL;
    if (first_slash != NULL) {
        second_slash = strchr(first_slash + 1, '/');
    }
    char* last_slash = strrchr(topic_str, '/');

    char* device_type;
    if (first_slash && second_slash && first_slash != second_slash) {
        device_type = strndup(first_slash + 1, second_slash - first_slash - 1);
        if (device_type == NULL) {
            ESP_LOGE(TAG, "OOM parsing MQTT topic");
            return;
        }
    } else {
        ESP_LOGW(TAG,
                 "MQTT topic format unexpected, cannot extract device type: %s",
                 topic_str);
        return;
    }

    if ((strcmp(device_type, "switch") == 0) ||
        (strcmp(device_type, "relay") == 0)) {
        ESP_LOGI(TAG, "Received non-JSON command for %s: %s", device_type,
                 topic_str);
    } else {
        ESP_LOGW(TAG, "Unknown device type in MQTT topic: %s", device_type);
        free(device_type);
        return;
    }

    uint64_t target_id = 0;

    if (last_slash && *(last_slash + 1) != '\0') {
        target_id = strtoull(last_slash + 1, NULL, 10);
    }

    if (data_len != 1 ||
        (data[0] != MSG_TYPE_PING && data[0] != MSG_TYPE_OTA_START)) {
        if (data_len > MESH_MSG_DATA_SIZE) {
            ESP_LOGW(TAG, "Command payload too large for mesh frame: %d > %d",
                     data_len, MESH_MSG_DATA_SIZE);
            free(device_type);
            return;
        }

        if (target_id == 0) {
            ESP_LOGW(TAG,
                     "Non-JSON command missing target device ID in topic: %s",
                     topic_str);
            free(device_type);
            return;
        }

        ESP_LOGI(TAG, "Non-JSON command for target device %" PRIu64, target_id);

        mesh_addr_t dest = {0};
        if (registry_find(target_id, &dest)) {
            mesh_app_msg_t cmd = {0};
            cmd.src_id = g_device_id;
            if (data_len == 1 && data[0] == MSG_TYPE_OTA_START)
                cmd.msg_type = MSG_TYPE_OTA_START;
            else
                cmd.msg_type = MSG_TYPE_COMMAND;

            memcpy(cmd.data, data, data_len);
            cmd.data_len = data_len;
            mesh_queue_to_node(&cmd, TX_PRIO_NORMAL, &dest);
            ESP_LOGI(TAG, "Routed non-JSON MQTT command to device %" PRIu64,
                     target_id);
        } else {
            ESP_LOGW(TAG, "Could not find target device %" PRIu64, target_id);
        }
    } else if (data_len == 1 && data[0] == MSG_TYPE_OTA_START) {
        ESP_LOGI(TAG, "Received OTA start command via MQTT");
        if (target_id == 0) {
            ESP_LOGW(TAG,
                     "OTA command missing target device ID in topic, "
                     "broadcasting: %s",
                     topic_str);
            mesh_app_msg_t ota_cmd = {0};
            ota_cmd.src_id = g_device_id;
            ota_cmd.msg_type = MSG_TYPE_OTA_START;
            ota_cmd.target_type = device_type[0] - 'a' + 'A';
            for (int i = 0; i < MAX_NODES; i++) {
                if (node_registry[i].device_id == 0) continue;
                mesh_queue_to_node(&ota_cmd, TX_PRIO_HIGH,
                                   &node_registry[i].mesh_addr);
                ESP_LOGI(TAG,
                         "Broadcasted OTA start command to device %" PRIu64,
                         node_registry[i].device_id);
            }
            free(device_type);
            return;
        }

        ESP_LOGI(TAG, "OTA start command for target device %" PRIu64,
                 target_id);
        mesh_addr_t dest = {0};
        if (registry_find(target_id, &dest)) {
            mesh_app_msg_t ota_cmd = {0};
            ota_cmd.src_id = g_device_id;
            ota_cmd.msg_type = MSG_TYPE_OTA_START;
            ota_cmd.target_type = device_type[0] - 'a' + 'A';
            mesh_queue_to_node(&ota_cmd, TX_PRIO_HIGH, &dest);
            ESP_LOGI(TAG, "Routed OTA start command to device %" PRIu64,
                     target_id);
        } else {
            ESP_LOGW(TAG, "Could not find target device %" PRIu64, target_id);
        }
    } else if (data_len == 1 && data[0] == MSG_TYPE_PING) {
        ESP_LOGI(TAG, "Received MQTT ping command");

        if (target_id == 0) {
            ESP_LOGW(TAG, "Ping command missing target device ID in topic.");
            free(device_type);
            return;
        }

        ESP_LOGI(TAG, "Ping command for target device %" PRIu64, target_id);
        mesh_addr_t dest = {0};
        if (registry_find(target_id, &dest)) {
            mesh_app_msg_t ping = {0};
            ping.src_id = g_device_id;
            ping.msg_type = MSG_TYPE_PING;
            uint16_t pingNum = 1;
            memcpy(ping.data, &pingNum, sizeof(uint16_t));
            ping.data_len = sizeof(uint16_t);

            if (mesh_queue_to_node(&ping, TX_PRIO_HIGH, &dest)) {
                if (xSemaphoreTake(registry_mutex, pdMS_TO_TICKS(5000)) ==
                    pdTRUE) {
                    int index = -1;
                    for (int i = 0; i < MAX_NODES; i++) {
                        if (node_registry[i].device_id == target_id) {
                            index = i;
                            break;
                        }
                    }
                    if (index >= 0) {
                        node_registry[index].last_ping =
                            esp_timer_get_time() / 1000;
                    }
                    xSemaphoreGive(registry_mutex);
                }
                ESP_LOGV(TAG, "Sent MQTT ping to device %" PRIu64, target_id);
            } else {
                ESP_LOGW(TAG, "Failed to enqueue ping for device %" PRIu64,
                         target_id);
            }
        } else {
            ESP_LOGW(TAG, "Could not find target device %" PRIu64, target_id);
        }
    } else {
        ESP_LOGW(TAG,
                 "Received unrecognized non-JSON MQTT command: topic=%s, "
                 "data=%.*s",
                 topic_str, data_len, data);
    }

    free(device_type);
}

// ====================
// Handle MQTT Commands
// ====================

/**
 * @brief Top-level MQTT command dispatcher.
 *
 * Routes /switch/cmd/root traffic to root-specific handlers (JSON or
 * non-JSON), and all other topic traffic to handle_nonJson_mqtt_command().
 */
static void handle_mqtt_command(const char* topic, int topic_len,
                                const char* data, int data_len) {
    if (data == NULL || data_len <= 0) {
        ESP_LOGW(TAG, "Ignoring empty MQTT payload on topic len=%d", topic_len);
        return;
    }

    if (data_len > MAX_MQTT_PAYLOAD_SIZE) {
        ESP_LOGW(TAG, "Ignoring MQTT payload larger than limit: %d > %d",
                 data_len, MAX_MQTT_PAYLOAD_SIZE);
        return;
    }

    char topic_str[128];
    int copy_len =
        (topic_len < sizeof(topic_str) - 1) ? topic_len : sizeof(topic_str) - 1;
    strncpy(topic_str, topic, copy_len);
    topic_str[copy_len] = '\0';

    if (strstr(topic_str, "/switch/cmd/root") != NULL) {
        ESP_LOGI(TAG, "Received root command");
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

// ====================
// Root Start / Stop
// ====================

/**
 * @brief Initialise root-only resources when this node becomes root.
 *        Creates the node registry mutex if it does not yet exist.
 *        Idempotent: returns immediately if the MQTT client is already running.
 */
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

/**
 * @brief Initialise and start the MQTT client (root node only).
 *
 * Builds the broker URI, generates a unique client ID from g_device_id,
 * prepares a Last Will and Testament (LWT) message, then starts the client.
 * Skipped silently when called on a non-root node.  The MQTT event handler
 * (mqtt_event_handler) manages subscriptions and further lifecycle events.
 */
void mqtt_init(void) {
    // Strong guard: Only root node should initialize MQTT
    if (!g_is_root) {
        ESP_LOGW(TAG,
                 "MQTT init called on NON-ROOT node (device_id: %" PRIu64
                 ", layer: %d) - skipping",
                 g_device_id, g_mesh_layer);
        ESP_LOGW(TAG,
                 "   This is expected for leaf nodes. Only root connects "
                 "to MQTT.");
        return;
    }

    ESP_LOGI(TAG,
             "Initializing MQTT client (ROOT node, device_id: %" PRIu64 ")",
             g_device_id);

    // Build complete MQTT broker URI
    char broker_uri[128];
    const char* url = CONFIG_MQTT_BROKER_URI;

    // Check if URL already includes port
    if (strstr(url, "mqtt://") != NULL && strchr(url + 7, ':') == NULL) {
        // No port in URL, append it
        snprintf(broker_uri, sizeof(broker_uri), "%s:%d", url, 1883);
    } else {
        // URL already complete or has port
        snprintf(broker_uri, sizeof(broker_uri), "%s", url);
    }

    // Generate unique MQTT client ID based on device ID
    // Store in static buffer so it persists after function returns
    snprintf(g_mqtt_client_id, sizeof(g_mqtt_client_id), "domator_%" PRIu64,
             g_device_id);
    ESP_LOGI(TAG, "Using MQTT client ID: %s", g_mqtt_client_id);

    snprintf(g_mqtt_lwt_message, sizeof(g_mqtt_lwt_message),
             "{\"status\":\"disconnected\",\"device_id\":%" PRIu64
             ",\"timestamp\":%" PRId64 ",\"reason\":\"ungraceful\"}",
             g_device_id, esp_timer_get_time() / 1000000);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.client_id = g_mqtt_client_id,
        .credentials.username = CONFIG_MQTT_USER,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
        .buffer.size = 4096,
        .buffer.out_size = 4096,
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
// MQTT Cleanup
// ====================

/**
 * @brief Stop the MQTT client and all root-specific services.
 *
 * If connected, publishes a "disconnected" status before stopping.
 * Destroys the MQTT client handle, stops the Telnet server, and clears
 * g_is_root.  Safe to call when already stopped.
 */
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

    telnet_stop();  // Stop telnet server if running

    g_is_root = false;  // Ensure we update root status
    ESP_LOGI(TAG, "Root services stopped");
}
