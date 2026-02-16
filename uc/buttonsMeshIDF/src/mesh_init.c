#include <stdlib.h>

#include "domator_mesh.h"

static const char* TAG = "MESH_INIT";

// Check whether the station network interface is up (we have an IP)
bool domator_mesh_is_wifi_connected(void) {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) return false;
    return esp_netif_is_netif_up(netif);
}

// ====================
// IP Event Handler
// ====================

static void ip_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        // Only initialize MQTT if this node is actually the root
        if (esp_mesh_is_root()) {
            ESP_LOGI(TAG, "This node IS root, initializing MQTT");
            g_is_root = true;
            g_mesh_layer = 1;

            // Initialize MQTT for root node (only if not already initialized)
            // This prevents multiple MQTT clients on repeated IP events
            extern esp_mqtt_client_handle_t g_mqtt_client;
            if (g_mqtt_client == NULL) {
                mqtt_init();
            } else {
                ESP_LOGI(TAG, "MQTT client already initialized, skipping");
            }
        } else {
            ESP_LOGI(TAG, "Got IP but not root (layer %d), skipping MQTT init",
                     g_mesh_layer);
        }
    }
}

static void mesh_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    switch (event_id) {
        case MESH_EVENT_STARTED:
            ESP_LOGI(TAG, "Mesh started");
            g_mesh_started = true;
            break;

        case MESH_EVENT_STOPPED:
            ESP_LOGI(TAG, "Mesh stopped");
            g_mesh_started = false;
            g_mesh_connected = false;
            break;

        case MESH_EVENT_PARENT_CONNECTED: {
            mesh_event_connected_t* connected =
                (mesh_event_connected_t*)event_data;
            ESP_LOGI(TAG, "Parent connected, layer:%d", connected->self_layer);
            g_mesh_connected = true;
            g_mesh_layer = connected->self_layer;

            if (esp_mesh_is_root()) {
                ESP_LOGI(TAG, "*** I AM ROOT ***");
                g_is_root = true;
                esp_netif_dhcpc_start(
                    esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
                node_root_start();
            } else {
                ESP_LOGI(TAG, "Not root (layer %d), ensuring MQTT is stopped",
                         g_mesh_layer);
                g_is_root = false;
                node_root_stop();
            }

            mesh_app_msg_t* msg = calloc(1, sizeof(mesh_app_msg_t));
            if (msg) {
                msg->src_id = g_device_id;
                msg->msg_type = MSG_TYPE_TYPE_INFO;
                char type_str = DEVICE_TYPE_SWITCH;
                if (g_node_type == NODE_TYPE_RELAY_8 ||
                    g_node_type == NODE_TYPE_RELAY_16) {
                    type_str = DEVICE_TYPE_RELAY;
                }

                msg->data_len = 1;
                msg->data[0] = type_str;
                mesh_queue_to_node(msg, TX_PRIO_NORMAL, NULL);
                free(msg);
            } else {
                ESP_LOGW(TAG,
                         "Failed to allocate msg for parent connected event");
            }

            ESP_LOGI(TAG,
                     "✓ Parent connected - Layer: %d, Mesh connected, status "
                     "reports will be sent to root",
                     g_mesh_layer);
            break;
        }

        case MESH_EVENT_PARENT_DISCONNECTED:
            mesh_event_disconnected_t* disconnected =
                (mesh_event_disconnected_t*)event_data;
            g_mesh_connected = false;

            ESP_LOGW(TAG, "Parent disconnected - Reason: %d",
                     disconnected->reason);

            if (g_stats_mutex &&
                xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_stats.mesh_disconnects++;
                xSemaphoreGive(g_stats_mutex);
            }
            break;

        case MESH_EVENT_TODS_STATE: {
            mesh_event_toDS_state_t* state =
                (mesh_event_toDS_state_t*)event_data;
            ESP_LOGI(TAG, "toDS state: %d", *state);
            break;
        }

        case MESH_EVENT_ROOT_SWITCH_REQ:
            ESP_LOGI(TAG, "Root switch requested");
            break;

        case MESH_EVENT_ROOT_SWITCH_ACK:
            g_is_root = esp_mesh_is_root();
            ESP_LOGI(TAG, "Root switched, am I root? %s",
                     g_is_root ? "YES" : "NO");
            if (g_is_root) {
                node_root_start();
            } else {
                node_root_stop();
            }
            break;

        case MESH_EVENT_CHILD_CONNECTED: {
            mesh_event_child_connected_t* child =
                (mesh_event_child_connected_t*)event_data;
            ESP_LOGI(TAG, "Child connected: " MACSTR, MAC2STR(child->mac));
            break;
        }

        case MESH_EVENT_CHILD_DISCONNECTED: {
            mesh_event_child_disconnected_t* child =
                (mesh_event_child_disconnected_t*)event_data;
            ESP_LOGW(TAG, "Child disconnected: " MACSTR, MAC2STR(child->mac));
            break;
        }

        default:
            ESP_LOGD(TAG, "Mesh event: %" PRId32, event_id);
            break;
    }
}

void mesh_stop_and_connect_sta(void) {
    ESP_LOGI(TAG, "Stopping mesh...");

    ESP_ERROR_CHECK(esp_mesh_stop());
    ESP_ERROR_CHECK(esp_mesh_deinit());

    ESP_ERROR_CHECK(esp_wifi_disconnect());

    wifi_config_t sta_config = {0};

    strcpy((char*)sta_config.sta.ssid, CONFIG_ROUTER_SSID);
    strcpy((char*)sta_config.sta.password, CONFIG_ROUTER_PASSWD);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Connecting to router for OTA...");

    while (1) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "Connected to router: %s", ap_info.ssid);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void mesh_network_init(void) {
    // WiFi init
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Disable WiFi power save to improve mesh performance
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // Mesh init
    ESP_ERROR_CHECK(esp_mesh_init());

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                               &mesh_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &ip_event_handler, NULL));

    // Mesh config
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();

    // Mesh ID
    uint8_t mesh_id[6] = CONFIG_MESH_ID;
    memcpy(&cfg.mesh_id, mesh_id, 6);

    // Router (your WiFi AP)
    cfg.channel = 0;  // auto-detect
    cfg.router.ssid_len = strlen(CONFIG_ROUTER_SSID);
    memcpy(cfg.router.ssid, CONFIG_ROUTER_SSID, cfg.router.ssid_len);
    memcpy(cfg.router.password, CONFIG_ROUTER_PASSWD,
           strlen(CONFIG_ROUTER_PASSWD));

    // Mesh softAP
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    memcpy(cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWD,
           strlen(CONFIG_MESH_AP_PASSWD));

    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));

    // Self-organized root election based on RSSI
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));

    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));

    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(0.9));

    // Start mesh
    ESP_ERROR_CHECK(esp_mesh_start());

    ESP_LOGI(TAG, "Mesh initialized, waiting for root election...");
}