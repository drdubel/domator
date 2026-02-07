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
            case 'C':
                // TODO: relay_handle_command when relay code is ported
                ESP_LOGI(TAG, "Command received: %.*s", msg->data_len,
                         msg->data);
                break;

            case 'U':
                ESP_LOGI(TAG, "OTA update requested");
                break;

            default:
                ESP_LOGW(TAG, "Unknown msg type: %c", msg->msg_type);
                break;
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

// ============ STATUS REPORT TASK ============
void status_report_task(void* arg) {
    vTaskDelay(pdMS_TO_TICKS(10000));  // wait for mesh to settle

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(15000));

        if (!g_mesh_connected) continue;

        ESP_LOGI(TAG, "Status: root=%d, connected=%d, heap=%" PRIu32, g_is_root,
                 g_mesh_connected, (uint32_t)esp_get_free_heap_size());

        // If we are root, publish status to MQTT
        if (g_is_root) {
            char payload[256];
            snprintf(payload, sizeof(payload),
                     "{\"deviceId\":%" PRIu32
                     ","
                     "\"type\":\"root\","
                     "\"freeHeap\":%" PRIu32
                     ","
                     "\"uptime\":%" PRIu32
                     ","
                     "\"meshLayer\":%d}",
                     g_device_id, (uint32_t)esp_get_free_heap_size(),
                     (uint32_t)(esp_timer_get_time() / 1000000),
                     esp_mesh_get_layer());

            extern void root_publish_status(const char* payload);
            root_publish_status(payload);
        }

        // If we are a leaf, send status over mesh to root
        if (!g_is_root && g_mesh_connected) {
            mesh_app_msg_t msg = {
                .src_id = g_device_id,
                .msg_type = 'S',
            };
            snprintf(msg.data, sizeof(msg.data),
                     "{\"deviceId\":%" PRIu32
                     ","
                     "\"type\":\"switch\","
                     "\"freeHeap\":%" PRIu32
                     ","
                     "\"uptime\":%" PRIu32 "}",
                     g_device_id, (uint32_t)esp_get_free_heap_size(),
                     (uint32_t)(esp_timer_get_time() / 1000000));
            msg.data_len = strlen(msg.data);

            mesh_queue_to_root(&msg);
        }
    }
}