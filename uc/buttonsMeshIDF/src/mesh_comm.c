/**
 * @file mesh_comm.c
 * @brief Mesh communication layer: RX/TX tasks, message queuing, and periodic
 *        status reporting.
 *
 * Provides:
 *  - mesh_send_to_node()   – thin wrapper around esp_mesh_send().
 *  - mesh_rx_task()        – receives packets and dispatches to root or leaf
 *                            handler.
 *  - mesh_tx_task()        – drains a queue of outbound packets.
 *  - mesh_queue_to_node()  – thread-safe enqueue for any task.
 *  - node_publish_status() – build and send a JSON status report.
 *  - status_report_task()  – periodic wrapper that calls the above.
 */

#include "cJSON.h"
#include "domator_mesh.h"
#include "esp_task_wdt.h"

static const char* TAG = "MESH_COMM";

// ====================
// Send to Specific Node
// ====================

/**
 * @brief Send a mesh application message to a specific node or to the root.
 * @param dest Destination mesh address.  Pass NULL to route to root.
 * @param msg  Pointer to the message structure to send.
 * @return ESP_OK on success, or an esp_err_t error code.
 */
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

// ====================
// RX Task
// ====================

/**
 * @brief Receive incoming mesh packets and dispatch them to the appropriate
 *        handler.
 *
 * When this node is root, all messages are forwarded to
 * root_handle_mesh_message().  Leaf nodes handle MSG_TYPE_COMMAND,
 * MSG_TYPE_SYNC_REQUEST, MSG_TYPE_OTA_START, and MSG_TYPE_PING directly.
 * Messages targeted at a device type that does not match this node are
 * silently discarded.
 */
void mesh_rx_task(void* arg) {
    mesh_addr_t from;
    mesh_data_t rx_data;
    uint8_t rx_buf[sizeof(mesh_app_msg_t) + 16];
    int flag;

    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "mesh_rx_task: esp_task_wdt_add failed: %s",
                 esp_err_to_name(wdt_err));
    }

    while (true) {
        if (g_ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_task_wdt_reset();
            continue;
        }

        rx_data.data = rx_buf;
        rx_data.size = sizeof(rx_buf);

        esp_err_t err =
            esp_mesh_recv(&from, &rx_data, pdMS_TO_TICKS(5000), &flag, NULL, 0);

        if (err == ESP_ERR_MESH_TIMEOUT) {
            esp_task_wdt_reset();
            continue;
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Mesh recv error: %s", esp_err_to_name(err));
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (rx_data.size < sizeof(mesh_app_msg_t)) {
            continue;
        }

        mesh_app_msg_t* msg = (mesh_app_msg_t*)rx_data.data;

        if ((msg->target_type == DEVICE_TYPE_RELAY &&
             g_node_type != NODE_TYPE_RELAY_8 &&
             g_node_type != NODE_TYPE_RELAY_16) ||
            (msg->target_type == DEVICE_TYPE_SWITCH &&
             g_node_type != NODE_TYPE_SWITCH_C3)) {
            ESP_LOGW(
                TAG,
                "Received message for device type %c, but I am not that type",
                msg->target_type);
            esp_task_wdt_reset();
            continue;
        }

        if (g_is_root) {
            root_handle_mesh_message(&from, msg);
            esp_task_wdt_reset();
            continue;
        }
        switch (msg->msg_type) {
            case MSG_TYPE_COMMAND: {
                ESP_LOGI(TAG, "Command received: %.*s", msg->data_len,
                         msg->data);

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

            case MSG_TYPE_OTA_START: {
                ESP_LOGI(TAG, "OTA update packet received from root");
                g_ota_requested = true;
                break;
            }

            case MSG_TYPE_PING: {
                ESP_LOGV(TAG, "Received ping from %" PRIu64, msg->src_id);

                mesh_app_msg_t pong = *msg;
                pong.src_id = g_device_id;
                pong.msg_type = MSG_TYPE_PING;
                mesh_queue_to_node(&pong, TX_PRIO_HIGH, &from);
                ESP_LOGV(TAG, "Sent pong to %" PRIu64, msg->src_id);
                break;
            }

            default: {
                ESP_LOGW(TAG, "Unknown msg type: %c", msg->msg_type);
                break;
            }
        }
        esp_task_wdt_reset();
    }
}

// ====================
// TX Task
// ====================

/** Internal item stored in the per-task TX queue. */
typedef struct {
    mesh_addr_t dest;
    mesh_app_msg_t* msg;
    bool to_root;
} tx_item_t;

static QueueHandle_t queue = NULL;

/**
 * @brief Drain the internal TX queue and forward each message via the mesh
 *        stack.  Pauses during OTA to avoid interfering with firmware writes.
 *        Allocates the internal queue on first invocation.
 */
void mesh_tx_task(void* arg) {
    queue = xQueueCreate(40, sizeof(tx_item_t*));

    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "mesh_tx_task: esp_task_wdt_add failed: %s",
                 esp_err_to_name(wdt_err));
    }

    tx_item_t* item;
    while (true) {
        if (g_ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_task_wdt_reset();
            continue;
        }

        if (xQueueReceive(queue, &item, pdMS_TO_TICKS(5000)) != pdTRUE) {
            esp_task_wdt_reset();
            continue;
        }

        if (item->to_root) {
            mesh_send_to_node(NULL, item->msg);
        } else {
            mesh_send_to_node(&item->dest, item->msg);
        }

        free(item->msg);
        free(item);

        vTaskDelay(pdMS_TO_TICKS(2));
        esp_task_wdt_reset();
    }
}

/**
 * @brief Enqueue a message for asynchronous transmission.
 *
 * Both the tx_item_t wrapper and the message payload are heap-allocated and
 * freed by mesh_tx_task() after delivery.
 *
 * @param msg  Source message (copied; the caller may reuse or free its copy).
 * @param prio Priority hint that controls queue position and timing:
 *             high-priority messages are queued at the front with more
 *             retries and longer wait times, while normal-priority messages
 *             are queued at the back with fewer retries and shorter waits.
 * @param dest Destination address, or NULL to send to the root node.
 *
 * @return true if the message was successfully enqueued; false if allocation
 *         fails or the queue cannot accept the message after the configured
 *         number of retry attempts.
 */
bool mesh_queue_to_node(mesh_app_msg_t* msg, tx_priority_t prio,
                        mesh_addr_t* dest) {
    tx_item_t* item = malloc(sizeof(tx_item_t));
    if (!item) {
        ESP_LOGE(TAG, "OOM queuing message");
        return false;
    }

    if (dest != NULL) {
        memcpy(&item->dest, dest, sizeof(mesh_addr_t));
        item->to_root = false;
    } else {
        memset(&item->dest, 0, sizeof(mesh_addr_t));
        item->to_root = true;
    }

    item->msg = malloc(sizeof(mesh_app_msg_t));
    if (!item->msg) {
        free(item);
        return false;
    }
    memcpy(item->msg, msg, sizeof(mesh_app_msg_t));

    BaseType_t sent = pdFALSE;
    const int max_attempts = (prio == TX_PRIO_HIGH) ? 2 : 2;
    for (int attempt = 0; attempt < max_attempts && sent != pdTRUE; attempt++) {
        // Keep high-priority enqueue non-blocking to avoid inflating ping RTT.
        TickType_t wait_ticks = (prio == TX_PRIO_HIGH) ? 0 : pdMS_TO_TICKS(100);
        if (prio == TX_PRIO_HIGH) {
            sent = xQueueSendToFront(queue, &item, wait_ticks);
        } else {
            sent = xQueueSendToBack(queue, &item, wait_ticks);
        }
    }

    if (sent != pdTRUE) {
        ESP_LOGW(TAG, "Queue full, dropping message (prio=%d, pending=%u)",
                 prio, (unsigned int)uxQueueMessagesWaiting(queue));
        free(item->msg);
        free(item);
        return false;
    }

    return true;
}

// ====================
// Node Status Report
// ====================

/**
 * @brief Build a JSON status payload and enqueue it toward the root node.
 *
 * Collects uptime, free heap, RSSI, mesh layer, firmware hash, and statistics
 * counters, serialises them as a compact JSON object, and places the result
 * into a MSG_TYPE_STATUS message.  Skipped when this node is root (the root
 * publishes its own status directly to MQTT via root_publish_status()).
 */
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

    uint32_t uptime = esp_timer_get_time() / 1000000;

    uint32_t free_heap = esp_get_free_heap_size();

    int rssi = 0;
    esp_wifi_sta_get_rssi(&rssi);

    if (free_heap < LOW_HEAP_THRESHOLD) {
        if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_stats.low_heap_events++;
            xSemaphoreGive(g_stats_mutex);
        }
    }

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

    ESP_LOGI(TAG, "Parent ID: %" PRIu64, g_parent_id);

    cJSON_AddNumberToObject(json, "deviceId", g_device_id);
    cJSON_AddStringToObject(json, "type", type_str);
    cJSON_AddNumberToObject(json, "parentId", g_parent_id);
    cJSON_AddNumberToObject(json, "freeHeap", free_heap);
    cJSON_AddNumberToObject(json, "uptime", uptime);
    cJSON_AddNumberToObject(json, "firmware", g_firmware_timestamp);
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
        msg.msg_type = MSG_TYPE_STATUS;
        msg.data_len = strlen(json_str);

        if (strlen(json_str) <= MESH_MSG_DATA_SIZE - 1) {
            memcpy(msg.data, json_str, msg.data_len);
            msg.data[msg.data_len] = '\0';

            mesh_queue_to_node(&msg, TX_PRIO_NORMAL, NULL);
        } else {
            ESP_LOGW(TAG, "Status report too large (%d bytes), max is %d",
                     msg.data_len, MESH_MSG_DATA_SIZE - 1);
        }

        free(json_str);
    }
}

// ====================
// Status Report Task
// ====================

/**
 * @brief Periodically publish a status report.
 *
 * Waits 5 seconds after start-up to let the mesh stabilise, then loops
 * every STATUS_REPORT_INTERVAL_MS.  Root nodes call root_publish_status();
 * leaf nodes call node_publish_status() to send a mesh message to the root.
 * Pauses during OTA.
 */
void status_report_task(void* arg) {
    ESP_LOGI(TAG, "Status report task started");

    vTaskDelay(pdMS_TO_TICKS(5000));

    while (true) {
        if (g_ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "Status: root=%d, connected=%d, heap=%" PRIu32, g_is_root,
                 g_mesh_connected, (uint32_t)esp_get_free_heap_size());

        if (g_is_root) {
            root_publish_status();
        }

        if (!g_is_root && g_mesh_connected) {
            node_publish_status();
        }

        vTaskDelay(pdMS_TO_TICKS(STATUS_REPORT_INTERVAL_MS));
    }
}
