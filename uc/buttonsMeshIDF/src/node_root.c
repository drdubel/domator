#include <inttypes.h>
#include <string.h>

#include "domator_mesh.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "mqtt_client.h"

static const char* TAG = "ROOT";
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

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
        case 'B': {
            char button = msg->data[0];
            int state = (msg->data_len > 1) ? msg->data[1] - '0' : -1;

            ESP_LOGI(TAG, "Button '%c' from switch %" PRIu32, button,
                     msg->src_id);

            if (mqtt_connected) {
                char topic[64];
                snprintf(topic, sizeof(topic), "/switch/state/%" PRIu32,
                         msg->src_id);
                char payload[2] = {button, '\0'};
                ESP_LOGI(TAG, "Publishing button status to MQTT: %s", payload);
                esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 0, 0);
            }

            route_button_to_relays(msg->src_id, button, state);
            break;
        }

        case 'R': {
            if (mqtt_connected) {
                char topic[64];
                snprintf(topic, sizeof(topic), "/switch/state/root");
                ESP_LOGI(TAG, "Publishing relay status to MQTT: %s", msg->data);
                esp_mqtt_client_publish(mqtt_client, topic, msg->data,
                                        msg->data_len, 0, 0);
            }
            break;
        }

        case 'S': {
            if (mqtt_connected) {
                char topic[64];
                snprintf(topic, sizeof(topic), "/switch/state/root");
                ESP_LOGI(TAG, "Publishing switch status to MQTT: %s",
                         msg->data);
                esp_mqtt_client_publish(mqtt_client, topic, msg->data,
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

void root_publish_status(const char* payload) {
    if (mqtt_client && mqtt_connected) {
        esp_mqtt_client_publish(mqtt_client, "/switch/state/root", payload, 0,
                                0, 0);
        ESP_LOGI(TAG, "MQTT published: %s", payload);
    }
}

// ============ MQTT EVENT HANDLER ============
static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                               int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            mqtt_connected = true;
            esp_mqtt_client_subscribe(mqtt_client, "/switch/cmd/+", 0);
            esp_mqtt_client_subscribe(mqtt_client, "/switch/cmd", 0);
            esp_mqtt_client_subscribe(mqtt_client, "/relay/cmd/+", 0);
            esp_mqtt_client_subscribe(mqtt_client, "/relay/cmd", 0);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            mqtt_connected = false;
            break;

        case MQTT_EVENT_DATA:
            handle_mqtt_command(event->topic, event->topic_len, event->data,
                                event->data_len);
            break;

        default:
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
    if (mqtt_client) return;

    if (!registry_mutex) {
        registry_mutex = xSemaphoreCreateMutex();
    }

    ESP_LOGI(TAG, "Starting root services...");
}

void node_root_mqtt_connect(void) {
    if (mqtt_client) return;

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
        .credentials.username = CONFIG_MQTT_USER,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    ESP_LOGI(TAG, "MQTT client started");
}

void node_root_stop(void) {
    if (mqtt_client) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        mqtt_connected = false;
    }
    ESP_LOGI(TAG, "Root services stopped");
}