#include <inttypes.h>
#include <string.h>

#include "domator_mesh.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

static const char* TAG = "HEALTH_OTA";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRIES 5

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_count = 0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, connecting...");
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disc =
            (wifi_event_sta_disconnected_t*)event_data;
        ESP_LOGW(TAG, "Disconnected, reason: %d", disc->reason);
        if (s_retry_count < WIFI_MAX_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "Retrying WiFi (%d/%d)...", s_retry_count,
                     WIFI_MAX_RETRIES);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t mesh_disconnect_and_ota(const char* ssid, const char* password,
                                  const char* ota_url) {
    esp_err_t ret;

    // ── 1. Stop mesh
    // ──────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Stopping mesh...");
    g_ota_in_progress =
        true;  // Signal all tasks to pause mesh activity during OTA

    // Ignore disconnect error — mesh may already be disconnected
    esp_mesh_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    ret = esp_mesh_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_mesh_stop failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Mesh stopped.");
    vTaskDelay(pdMS_TO_TICKS(500));

    // ── 2. Fully stop WiFi and destroy mesh netifs
    // ──────────────────────────── The mesh stack leaves WiFi in AP+STA mode
    // with mesh-owned netifs. We must stop WiFi, destroy those netifs, then
    // rebuild cleanly.
    ESP_LOGI(TAG, "Tearing down WiFi and mesh netifs...");

    ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    ret = esp_wifi_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_deinit: %s", esp_err_to_name(ret));
    }

    // Destroy ALL existing netifs so mesh-owned ones are gone
    // Iterate and destroy every netif except the loopback
    esp_netif_t* netif = esp_netif_next_unsafe(NULL);
    while (netif != NULL) {
        esp_netif_t* next = esp_netif_next_unsafe(netif);
        const char* key = esp_netif_get_ifkey(netif);
        // Skip loopback; destroy everything else (WIFI_STA_DEF, WIFI_AP_DEF,
        // mesh netifs)
        if (strcmp(key, "lo") != 0) {
            ESP_LOGI(TAG, "Destroying netif: %s", key);
            esp_netif_destroy(netif);
        }
        netif = next;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    // ── 3. Re-init WiFi as plain STA
    // ──────────────────────────────────────────
    ESP_LOGI(TAG, "Re-initialising WiFi as plain STA...");

    // Create a fresh default STA netif
    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(ret));
        return ret;
    }

    // ── 4. Register event handlers
    // ────────────────────────────────────────────
    s_wifi_event_group = xEventGroupCreate();
    s_retry_count = 0;

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
        &instance_got_ip));

    // ── 5. Configure and start STA
    // ────────────────────────────────────────────
    wifi_config_t wifi_cfg = {
        .sta =
            {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg =
                    {
                        .capable = true,
                        .required = false,
                    },
            },
    };
    strlcpy((char*)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char*)wifi_cfg.sta.password, password,
            sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());  // → triggers STA_START → connect

    // ── 6. Wait for IP
    // ────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Waiting for WiFi connection to '%s'...", ssid);
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE,
        pdFALSE, pdMS_TO_TICKS(30000));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", ssid);
        ret = ESP_FAIL;
        esp_restart();
    }

    // ── 7. HTTPS OTA
    // ──────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Starting OTA from: %s", ota_url);

    esp_http_client_config_t http_cfg = {
        .url = ota_url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    ret = esp_https_ota(&ota_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA complete! Rebooting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
        esp_restart();
    }
}

void ota_task(void* arg) {
    uint32_t ota_countdown = 0;
    bool ota_countdown_active = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (g_ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (g_ota_requested) {
            g_ota_requested = false;
            ota_countdown_active = true;
        }

        if (!ota_countdown_active) {
            ota_countdown = esp_timer_get_time() / 1000;  // ms
            continue;
        } else if ((esp_timer_get_time() / 1000) - ota_countdown >=
                   OTA_COUNTDOWN_MS) {
            ota_countdown_active = false;
            ESP_LOGI(TAG, "OTA countdown complete, starting OTA...");
            mesh_disconnect_and_ota(CONFIG_ROUTER_SSID, CONFIG_ROUTER_PASSWD,
                                    CONFIG_OTA_URL);
        }
    }
}

// ==================== Health Monitoring ====================
void health_monitor_task(void* arg) {
    ESP_LOGI(TAG, "Health monitor task started");

    uint32_t last_low_heap_log = 0;
    uint32_t last_critical_heap_log = 0;

    while (1) {
        if (g_ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 seconds
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 seconds

        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t current_time = esp_timer_get_time() / 1000;  // ms

        // Check for low heap
        if (free_heap < LOW_HEAP_THRESHOLD) {
            // Log at most once per minute
            if (current_time - last_low_heap_log > 60000) {
                ESP_LOGW(TAG, "Low heap detected: %lu bytes free", free_heap);
                last_low_heap_log = current_time;

                if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) ==
                    pdTRUE) {
                    g_stats.low_heap_events++;
                    xSemaphoreGive(g_stats_mutex);
                }
            }
        }

        // Check for critical heap
        if (free_heap < CRITICAL_HEAP_THRESHOLD) {
            // Log at most once per minute
            if (current_time - last_critical_heap_log > 60000) {
                ESP_LOGE(TAG, "CRITICAL heap level: %lu bytes free", free_heap);
                last_critical_heap_log = current_time;

                if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) ==
                    pdTRUE) {
                    g_stats.critical_heap_events++;
                    xSemaphoreGive(g_stats_mutex);
                }
            }
        }
    }
}