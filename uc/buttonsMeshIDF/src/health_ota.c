/**
 * @file health_ota.c
 * @brief Device health monitoring and Over-The-Air (OTA) firmware update.
 *
 * Health monitoring:
 *  - health_monitor_task() runs every 5 seconds, logs warnings when free heap
 *    falls below LOW_HEAP_THRESHOLD or CRITICAL_HEAP_THRESHOLD, and increments
 *    the corresponding statistics counters.
 *
 * OTA update flow:
 *  1. Any node can request an OTA by setting g_ota_requested (via mesh message
 *     or MQTT command).  ota_task() monitors this flag.
 *  2. After a short countdown (OTA_COUNTDOWN_MS) to drain in-flight messages,
 *     mesh_disconnect_and_ota() is called.
 *  3. The mesh and WiFi stacks are torn down cleanly, the device reconnects as
 *     a plain STA, downloads the firmware via HTTPS OTA, and reboots.
 */

#include <inttypes.h>
#include <string.h>

#include "domator_mesh.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "nvs.h"

static const char* TAG = "HEALTH_OTA";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRIES 5

#define NVS_OTA_NAMESPACE "ota_state"
#define NVS_OTA_FAIL_KEY "fail_count"

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_count = 0;

// ====================
// WiFi Event Handler (OTA)
// ====================

/**
 * @brief Handle WiFi and IP events used exclusively during OTA Wi-Fi setup.
 *        Retries connection up to WIFI_MAX_RETRIES times, then signals failure
 *        via the s_wifi_event_group event bits.
 */
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

// ====================
// OTA Failure Counter (NVS-persisted)
// ====================

/**
 * @brief Read the OTA failure counter from NVS.
 * @return Current failure count, or 0 if not set.
 */
static uint8_t ota_get_fail_count(void) {
    nvs_handle_t handle;
    uint8_t count = 0;
    esp_err_t err = nvs_open(NVS_OTA_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_u8(handle, NVS_OTA_FAIL_KEY, &count);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "ota_get_fail_count: nvs_get_u8 error %s", esp_err_to_name(err));
        }
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "ota_get_fail_count: nvs_open error %s", esp_err_to_name(err));
    }
    return count;
}

/**
 * @brief Write the OTA failure counter to NVS.
 * @param count New failure count value.
 */
static void ota_set_fail_count(uint8_t count) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_OTA_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, NVS_OTA_FAIL_KEY, count);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_set_fail_count: nvs_set_u8 error %s", esp_err_to_name(err));
        } else {
            err = nvs_commit(handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "ota_set_fail_count: nvs_commit error %s", esp_err_to_name(err));
            }
        }
        nvs_close(handle);
    } else {
        ESP_LOGE(TAG, "ota_set_fail_count: nvs_open error %s", esp_err_to_name(err));
    }
}

/**
 * @brief Reset the OTA failure counter to zero (called after successful OTA).
 */
static void ota_reset_fail_count(void) {
    ota_set_fail_count(0);
}

// ====================
// Mesh Teardown and OTA
// ====================

/**
 * @brief Stop the mesh stack, switch to plain STA mode, and perform HTTPS OTA.
 *
 * This function does not return on success (esp_restart() is called after a
 * successful flash).  On WiFi connection failure or OTA error, the device also
 * restarts to recover gracefully.
 *
 * @param ssid     SSID of the router to connect to for the OTA download.
 * @param password Router WiFi password.
 * @param ota_url  HTTPS URL of the firmware binary.
 * @return Never returns on success.  Returns ESP_FAIL if WiFi setup fails
 *         before the restart call is reached.
 */
esp_err_t mesh_disconnect_and_ota(const char* ssid, const char* password,
                                  const char* ota_url) {
    esp_err_t ret;

    ESP_LOGI(TAG, "Stopping mesh...");
    g_ota_in_progress = true;

    esp_mesh_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    ret = esp_mesh_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_mesh_stop failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Mesh stopped.");
    vTaskDelay(pdMS_TO_TICKS(500));

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

    esp_netif_t* netif = esp_netif_next_unsafe(NULL);
    while (netif != NULL) {
        esp_netif_t* next = esp_netif_next_unsafe(netif);
        const char* key = esp_netif_get_ifkey(netif);
        if (strcmp(key, "lo") != 0) {
            ESP_LOGI(TAG, "Destroying netif: %s", key);
            esp_netif_destroy(netif);
        }
        netif = next;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "Re-initialising WiFi as plain STA...");

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
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for WiFi connection to '%s'...", ssid);
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE,
        pdFALSE, pdMS_TO_TICKS(30000));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", ssid);
        ret = ESP_FAIL;
        esp_restart();
    }

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
        ESP_LOGI(TAG, "OTA complete! Resetting failure counter and rebooting...");
        ota_reset_fail_count();
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
        uint8_t fail_count = ota_get_fail_count() + 1;
        ota_set_fail_count(fail_count);
        ESP_LOGE(TAG, "OTA failure count: %d / %d", fail_count, OTA_MAX_FAILURES);

        if (fail_count >= OTA_MAX_FAILURES) {
            ESP_LOGE(TAG, "Max OTA failures reached (%d). Rolling back to previous firmware...",
                     OTA_MAX_FAILURES);
            ota_reset_fail_count();
            esp_ota_mark_app_invalid_rollback_and_reboot();
            // Does not return
        }

        ESP_LOGW(TAG, "Rebooting to retry OTA later...");
        esp_restart();
    }
}

/**
 * @brief FreeRTOS task: watch for g_ota_requested and trigger OTA after
 *        a countdown.
 */
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

            uint8_t fail_count = ota_get_fail_count();
            if (fail_count >= OTA_MAX_FAILURES) {
                ESP_LOGE(TAG, "OTA blocked: %d consecutive failures reached limit. "
                         "Rolling back to previous firmware...", fail_count);
                ota_reset_fail_count();
                esp_ota_mark_app_invalid_rollback_and_reboot();
                continue;
            }

            ota_countdown_active = true;
        }

        if (!ota_countdown_active) {
            ota_countdown = esp_timer_get_time() / 1000;
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

// ====================
// Health Monitoring
// ====================

/**
 * @brief Check OTA failure counter at boot and rollback if limit exceeded.
 *        Call this early in app_main() to catch reboot loops from failed OTA.
 */
void ota_check_rollback_on_boot(void) {
    uint8_t fail_count = ota_get_fail_count();
    if (fail_count >= OTA_MAX_FAILURES) {
        ESP_LOGE(TAG, "Boot: OTA failure count %d >= %d. Rolling back to previous firmware...",
                 fail_count, OTA_MAX_FAILURES);
        ota_reset_fail_count();
        esp_ota_mark_app_invalid_rollback_and_reboot();
        // Does not return
    }
}

/**
 * @brief FreeRTOS task: monitor free heap every 5 seconds and update
 *        low/critical heap statistics.
 */
void health_monitor_task(void* arg) {
    ESP_LOGI(TAG, "Health monitor task started");

    uint32_t last_low_heap_log = 0;
    uint32_t last_critical_heap_log = 0;

    while (1) {
        if (g_ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(5000));

        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t current_time = esp_timer_get_time() / 1000;

        if (free_heap < LOW_HEAP_THRESHOLD) {
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

        if (free_heap < CRITICAL_HEAP_THRESHOLD) {
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