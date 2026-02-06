#include "domator_mesh.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "MESH_INIT";

// ====================
// WiFi Event Handler
// ====================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started");
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "WiFi STA disconnected");
                if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    g_stats.mesh_disconnects++;
                    xSemaphoreGive(g_stats_mutex);
                }
                break;
            default:
                break;
        }
    }
}

// ====================
// IP Event Handler  
// ====================

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Root got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        g_is_root = true;
        g_mesh_layer = 1;
        
        // Initialize MQTT for root node
        mqtt_init();
    }
}

// ====================
// Mesh Event Handler
// ====================

static void mesh_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    mesh_event_t event = (mesh_event_t)event_id;
    
    switch (event) {
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
            mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
            g_mesh_connected = true;
            g_mesh_layer = connected->self_layer;
            
            // For now, we'll derive parent ID from MAC when needed
            // The connected structure contains parent MAC info
            
            ESP_LOGI(TAG, "Parent connected - Layer: %d", g_mesh_layer);
            break;
        }
        
        case MESH_EVENT_PARENT_DISCONNECTED: {
            mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
            g_mesh_connected = false;
            
            ESP_LOGW(TAG, "Parent disconnected - Reason: %d", disconnected->reason);
            
            if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_stats.mesh_disconnects++;
                xSemaphoreGive(g_stats_mutex);
            }
            break;
        }
        
        case MESH_EVENT_CHILD_CONNECTED: {
            mesh_event_child_connected_t *child = (mesh_event_child_connected_t *)event_data;
            ESP_LOGI(TAG, "Child connected - MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     child->mac[0], child->mac[1], child->mac[2],
                     child->mac[3], child->mac[4], child->mac[5]);
            break;
        }
        
        case MESH_EVENT_CHILD_DISCONNECTED: {
            mesh_event_child_disconnected_t *child = (mesh_event_child_disconnected_t *)event_data;
            ESP_LOGI(TAG, "Child disconnected - MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     child->mac[0], child->mac[1], child->mac[2],
                     child->mac[3], child->mac[4], child->mac[5]);
            break;
        }
        
        case MESH_EVENT_ROUTING_TABLE_ADD:
        case MESH_EVENT_ROUTING_TABLE_REMOVE:
            // Routing table changes
            break;
            
        case MESH_EVENT_ROOT_ADDRESS: {
            mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)event_data;
            ESP_LOGI(TAG, "Root address: %02X:%02X:%02X:%02X:%02X:%02X",
                     root_addr->addr[0], root_addr->addr[1], root_addr->addr[2],
                     root_addr->addr[3], root_addr->addr[4], root_addr->addr[5]);
            break;
        }
        
        case MESH_EVENT_TODS_STATE: {
            mesh_event_toDS_state_t *toDS = (mesh_event_toDS_state_t *)event_data;
            ESP_LOGI(TAG, "Root toDS state: %d", *toDS);
            break;
        }
        
        case MESH_EVENT_ROOT_FIXED:
            ESP_LOGI(TAG, "Root fixed");
            break;
            
        case MESH_EVENT_ROOT_ASKED_YIELD:
            ESP_LOGI(TAG, "Root asked to yield");
            break;
            
        default:
            ESP_LOGD(TAG, "Mesh event: %d", event);
            break;
    }
}

// ====================
// WiFi Initialization
// ====================

void wifi_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi");
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create network interfaces
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &ip_event_handler, NULL));
    
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ====================
// Mesh Initialization
// ====================

void mesh_init(void)
{
    ESP_LOGI(TAG, "Initializing mesh network");
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize WiFi
    wifi_init();
    
    // Initialize mesh
    ESP_ERROR_CHECK(esp_mesh_init());
    
    // Register mesh event handler
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                                &mesh_event_handler, NULL));
    
    // Set mesh ID from config
    const char *mesh_id_str = CONFIG_MESH_ID;
    mesh_addr_t mesh_id;
    if (strlen(mesh_id_str) >= 6) {
        memcpy(mesh_id.addr, mesh_id_str, 6);
    } else {
        // Default mesh ID
        memcpy(mesh_id.addr, "DMESH0", 6);
    }
    ESP_ERROR_CHECK(esp_mesh_set_id(&mesh_id));
    
    // Configure mesh
    mesh_cfg_t mesh_cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t *)&mesh_cfg.mesh_id, mesh_id.addr, 6);
    mesh_cfg.channel = 0;  // Auto channel selection
    mesh_cfg.router.ssid_len = strlen(CONFIG_WIFI_SSID);
    memcpy((uint8_t *)&mesh_cfg.router.ssid, CONFIG_WIFI_SSID, mesh_cfg.router.ssid_len);
    memcpy((uint8_t *)&mesh_cfg.router.password, CONFIG_WIFI_PASSWORD, strlen(CONFIG_WIFI_PASSWORD));
    mesh_cfg.mesh_ap.max_connection = 6;
    mesh_cfg.mesh_ap.nonmesh_max_connection = 0;
    
    ESP_ERROR_CHECK(esp_mesh_set_config(&mesh_cfg));
    
    // Set mesh topology to tree
    ESP_ERROR_CHECK(esp_mesh_set_topology(MESH_TOPO_TREE));
    
    // Allow root node switching
    ESP_ERROR_CHECK(esp_mesh_set_root_healing_delay(10000));
    ESP_ERROR_CHECK(esp_mesh_allow_root_conflicts(true));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(0.9));
    
    // Set self-organized
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));
    
    // Start mesh
    ESP_ERROR_CHECK(esp_mesh_start());
    
    ESP_LOGI(TAG, "Mesh initialized - SSID: %s, Mesh ID: %02X%02X%02X%02X%02X%02X",
             CONFIG_WIFI_SSID, 
             mesh_id.addr[0], mesh_id.addr[1], mesh_id.addr[2],
             mesh_id.addr[3], mesh_id.addr[4], mesh_id.addr[5]);
}
