#include "cJSON.h"
#include "domator_mesh.h"

static const char* TAG = "MESH_COMM";

// ============ SEND TO ROOT ============
esp_err_t mesh_send_to_root(mesh_app_msg_t* msg) {
    mesh_data_t data = {
        .data = (uint8_t*)msg,
        .size = sizeof(mesh_app_msg_t),
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_P2P,
    };

    esp_err_t err = esp_mesh_send(NULL, &data, MESH_DATA_P2P, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Send to root failed: %s", esp_err_to_name(err));
    }
    return err;
}

// ============ SEND TO SPECIFIC NODE ============
esp_err_t mesh_send_to_node(mesh_addr_t* dest, mesh_app_msg_t* msg) {
    mesh_data_t data = {
        .data = (uint8_t*)msg,
        .size = sizeof(mesh_app_msg_t),
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_P2P,
    };

    esp_err_t err = esp_mesh_send(dest, &data, MESH_DATA_P2P, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Send to node failed: %s", esp_err_to_name(err));
    }
    return err;
}

// ============ RX TASK ============
void mesh_rx_task(void* arg) {
    mesh_addr_t from;
    mesh_data_t rx_data;
    uint8_t rx_buf[sizeof(mesh_app_msg_t) + 16];
    int flag;

    while (true) {
        rx_data.data = rx_buf;
        rx_data.size = sizeof(rx_buf);

        esp_err_t err =
            esp_mesh_recv(&from, &rx_data, portMAX_DELAY, &flag, NULL, 0);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Mesh recv error: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (rx_data.size < sizeof(mesh_app_msg_t)) {
            continue;
        }

        mesh_app_msg_t* msg = (mesh_app_msg_t*)rx_data.data;

        // If we are root, handle as root
        if (g_is_root) {
            root_handle_mesh_message(&from, msg);
            continue;
        }

        // Leaf node handling
        switch (msg->msg_type) {
            case MSG_TYPE_COMMAND: {
                // TODO: relay_handle_command when relay code is ported
                ESP_LOGI(TAG, "Command received: %.*s", msg->data_len,
                         msg->data);
                break;
            }

            case MSG_TYPE_OTA_TRIGGER: {
                ESP_LOGI(TAG, "OTA update requested");
                break;
            }

            case MSG_TYPE_PING: {
                ESP_LOGV(TAG, "Received ping from %" PRIu32, msg->src_id);

                mesh_app_msg_t pong = *msg;
                pong.src_id = g_device_id;
                pong.msg_type = MSG_TYPE_PING;
                mesh_queue_to_node(&from, &pong);
                ESP_LOGV(TAG, "Sent pong to %" PRIu32, msg->src_id);
                break;
            }

            default: {
                ESP_LOGW(TAG, "Unknown msg type: %c", msg->msg_type);
                break;
            }
        }
    }
}

// ============ TX TASK ============
typedef struct {
    mesh_addr_t dest;
    mesh_app_msg_t msg;
    bool to_root;
} tx_item_t;

static QueueHandle_t tx_queue = NULL;

void mesh_tx_task(void* arg) {
    tx_queue = xQueueCreate(20, sizeof(tx_item_t));

    tx_item_t item;
    while (true) {
        if (xQueueReceive(tx_queue, &item, portMAX_DELAY) == pdTRUE) {
            if (item.to_root) {
                mesh_send_to_root(&item.msg);
            } else {
                mesh_send_to_node(&item.dest, &item.msg);
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

void mesh_queue_to_root(mesh_app_msg_t* msg) {
    if (!tx_queue) return;
    tx_item_t item = {.to_root = true};
    memset(&item.dest, 0, sizeof(mesh_addr_t));
    memcpy(&item.msg, msg, sizeof(mesh_app_msg_t));
    xQueueSend(tx_queue, &item, pdMS_TO_TICKS(100));
}

void mesh_queue_to_node(mesh_addr_t* dest, mesh_app_msg_t* msg) {
    if (!tx_queue) return;
    tx_item_t item = {.to_root = false};
    memcpy(&item.dest, dest, sizeof(mesh_addr_t));
    memcpy(&item.msg, msg, sizeof(mesh_app_msg_t));
    xQueueSend(tx_queue, &item, pdMS_TO_TICKS(100));
}

void node_publish_status(void) {
    if (g_is_root) {
        return;
    }

    cJSON* json = cJSON_CreateObject();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        vTaskDelay(pdMS_TO_TICKS(STATUS_REPORT_INTERVAL_MS));
        return;
    }

    // Get uptime in seconds
    uint32_t uptime = esp_timer_get_time() / 1000000;

    // Get free heap
    uint32_t free_heap = esp_get_free_heap_size();

    // Get RSSI from parent connection
    int rssi = 0;
    esp_wifi_sta_get_rssi(&rssi);

    // Check for low heap
    if (free_heap < LOW_HEAP_THRESHOLD) {
        if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_stats.low_heap_events++;
            xSemaphoreGive(g_stats_mutex);
        }
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

    mesh_addr_t parent_addr;

    esp_err_t err = esp_mesh_get_parent_bssid(&parent_addr);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Parent MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 parent_addr.addr[0], parent_addr.addr[1], parent_addr.addr[2],
                 parent_addr.addr[3], parent_addr.addr[4], parent_addr.addr[5]);
    } else {
        ESP_LOGW(TAG, "Failed to get parent BSSID: %s", esp_err_to_name(err));
    }

    uint32_t parent_id =
        ((parent_addr.addr[2] << 24) | (parent_addr.addr[3] << 16) |
         (parent_addr.addr[4] << 8) | parent_addr.addr[5]) -
        1;

    ESP_LOGI(TAG, "Parent ID: %" PRIu32, parent_id);

    // Build JSON status report
    cJSON_AddNumberToObject(json, "deviceId", g_device_id);
    cJSON_AddStringToObject(json, "type", type_str);
    cJSON_AddNumberToObject(json, "parentId", parent_id);
    cJSON_AddNumberToObject(json, "freeHeap", free_heap);
    cJSON_AddNumberToObject(json, "uptime", uptime);
    cJSON_AddStringToObject(json, "firmware", g_firmware_hash);
    cJSON_AddNumberToObject(json, "clicks", g_stats.button_presses);
    cJSON_AddNumberToObject(json, "rssi", rssi);
    cJSON_AddNumberToObject(json, "meshLayer", g_mesh_layer);
    cJSON_AddNumberToObject(json, "disconnects", g_stats.mesh_disconnects);
    cJSON_AddNumberToObject(json, "lowHeap", g_stats.low_heap_events);

    char* json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (json_str != NULL) {
        mesh_app_msg_t msg = {0};
        msg.src_id = g_device_id;
        msg.msg_type = MSG_TYPE_STATUS;  // Status message
        msg.data_len = strlen(json_str);

        if (strlen(json_str) <= MESH_MSG_DATA_SIZE - 1) {
            memcpy(msg.data, json_str, msg.data_len);
            msg.data[msg.data_len] = '\0';

            mesh_queue_to_root(&msg);
        } else {
            ESP_LOGW(TAG, "Status report too large (%d bytes), max is %d",
                     msg.data_len, MESH_MSG_DATA_SIZE - 1);
        }

        free(json_str);
    }
}

// ============ STATUS REPORT TASK ============
void status_report_task(void* arg) {
    ESP_LOGI(TAG, "Status report task started");

    // Wait for mesh to stabilize
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (true) {
        if (g_ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "Status: root=%d, connected=%d, heap=%" PRIu32, g_is_root,
                 g_mesh_connected, (uint32_t)esp_get_free_heap_size());

        // If we are root, publish status to MQTT
        if (g_is_root) {
            root_publish_status();
        }

        // If we are a leaf, send status over mesh to root
        if (!g_is_root && g_mesh_connected) {
            node_publish_status();
        }

        vTaskDelay(pdMS_TO_TICKS(STATUS_REPORT_INTERVAL_MS));
    }
}