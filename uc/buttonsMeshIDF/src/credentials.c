/**
 * @file credentials.c
 * @brief Load per-installation settings from NVS.
 *
 * See credentials.h for why none of this is compiled in. Provisioning is
 * described in provisioning/README.md.
 */

#include "credentials.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "CREDS";

static domator_credentials_t s_creds = {0};
static bool s_loaded = false;

// NVS keys. Each must stay within the 15-character NVS key limit.
#define KEY_ROUTER_SSID "router_ssid"
#define KEY_ROUTER_PASS "router_pass"
#define KEY_MESH_ID "mesh_id"
#define KEY_MESH_AP_PASS "mesh_ap_pass"
#define KEY_MQTT_URI "mqtt_uri"
#define KEY_MQTT_USER "mqtt_user"
#define KEY_MQTT_PASS "mqtt_pass"
#define KEY_OTA_URL "ota_url"
#define KEY_OTA_TOKEN "ota_token"

/**
 * @brief Copy one string value out of NVS.
 *
 * @param required When true, a missing key is an error. When false, a missing
 *                 key leaves @p out empty and still returns ESP_OK.
 * @return ESP_OK, ESP_ERR_NVS_NOT_FOUND for a missing required key, or
 *         ESP_ERR_INVALID_SIZE when the stored value does not fit.
 */
static esp_err_t read_str(nvs_handle_t handle, const char* key, char* out,
                          size_t out_size, bool required) {
    size_t len = 0;
    esp_err_t err = nvs_get_str(handle, key, NULL, &len);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
        if (required) {
            ESP_LOGE(TAG, "Missing required key '%s' in NVS namespace '%s'",
                     key, DOMATOR_NVS_NAMESPACE);
            return ESP_ERR_NVS_NOT_FOUND;
        }
        return ESP_OK;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reading '%s' failed: %s", key, esp_err_to_name(err));
        return err;
    }

    if (len > out_size) {
        ESP_LOGE(TAG, "Value for '%s' is %u bytes, buffer holds %u", key,
                 (unsigned)len, (unsigned)out_size);
        return ESP_ERR_INVALID_SIZE;
    }

    err = nvs_get_str(handle, key, out, &len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reading '%s' failed: %s", key, esp_err_to_name(err));
    }

    return err;
}

esp_err_t credentials_load(void) {
    esp_err_t err = nvs_flash_init_partition(DOMATOR_NVS_PARTITION);

    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG,
                 "No '%s' partition on this device. Flash the current "
                 "partition table (partitions.csv), then provision.",
                 DOMATOR_NVS_PARTITION);
        return ESP_ERR_NVS_NOT_FOUND;
    }

    if (err != ESP_OK) {
        // Deliberately NOT erasing on corruption: that would destroy the
        // credentials and strand the device until someone brings a cable.
        ESP_LOGE(TAG, "Cannot mount '%s' partition: %s", DOMATOR_NVS_PARTITION,
                 esp_err_to_name(err));
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open_from_partition(DOMATOR_NVS_PARTITION, DOMATOR_NVS_NAMESPACE,
                                  NVS_READONLY, &handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open NVS namespace '%s' in '%s': %s",
                 DOMATOR_NVS_NAMESPACE, DOMATOR_NVS_PARTITION,
                 esp_err_to_name(err));
        return ESP_ERR_NVS_NOT_FOUND;
    }

    memset(&s_creds, 0, sizeof(s_creds));

    struct {
        const char* key;
        char* out;
        size_t size;
        bool required;
    } fields[] = {
        {KEY_ROUTER_SSID, s_creds.router_ssid, sizeof(s_creds.router_ssid),
         true},
        {KEY_ROUTER_PASS, s_creds.router_pass, sizeof(s_creds.router_pass),
         true},
        {KEY_MESH_ID, s_creds.mesh_id, sizeof(s_creds.mesh_id), true},
        {KEY_MESH_AP_PASS, s_creds.mesh_ap_pass, sizeof(s_creds.mesh_ap_pass),
         true},
        {KEY_MQTT_URI, s_creds.mqtt_uri, sizeof(s_creds.mqtt_uri), true},
        {KEY_MQTT_USER, s_creds.mqtt_user, sizeof(s_creds.mqtt_user), true},
        {KEY_MQTT_PASS, s_creds.mqtt_pass, sizeof(s_creds.mqtt_pass), true},
        // OTA is optional: a node without it simply cannot self-update.
        {KEY_OTA_URL, s_creds.ota_url, sizeof(s_creds.ota_url), false},
        {KEY_OTA_TOKEN, s_creds.ota_token, sizeof(s_creds.ota_token), false},
    };

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        err = read_str(handle, fields[i].key, fields[i].out, fields[i].size,
                       fields[i].required);
        if (err != ESP_OK) {
            nvs_close(handle);
            return err;
        }
    }

    nvs_close(handle);

    // The mesh ID is exactly 6 bytes on the wire. Longer values are truncated
    // rather than rejected, because that is what the previous CONFIG_MESH_ID
    // code did (memcpy of sizeof(mesh_id) == 6): deployed meshes are already
    // formed around those first 6 bytes, and tightening this now would split
    // the mesh. Shorter values genuinely cannot work.
    size_t mesh_id_len = strlen(s_creds.mesh_id);

    if (mesh_id_len < DOMATOR_MESH_ID_LEN) {
        ESP_LOGE(TAG, "'%s' must be at least %d characters, got %u",
                 KEY_MESH_ID, DOMATOR_MESH_ID_LEN, (unsigned)mesh_id_len);
        return ESP_ERR_INVALID_SIZE;
    }

    if (mesh_id_len > DOMATOR_MESH_ID_LEN) {
        ESP_LOGW(TAG, "'%s' is %u characters; only the first %d are used ('%s')",
                 KEY_MESH_ID, (unsigned)mesh_id_len, DOMATOR_MESH_ID_LEN,
                 s_creds.mesh_id);
    }

    s_loaded = true;

    return ESP_OK;
}

bool credentials_are_loaded(void) { return s_loaded; }

const domator_credentials_t* credentials_get(void) { return &s_creds; }

void credentials_log_summary(void) {
    // Values that identify the installation are logged; secrets are reduced to
    // a length, because the log stream is mirrored over telnet to anyone on
    // the LAN.
    ESP_LOGI(TAG, "Router SSID: %s (password: %u chars)", s_creds.router_ssid,
             (unsigned)strlen(s_creds.router_pass));
    ESP_LOGI(TAG, "Mesh ID: %s (AP password: %u chars)", s_creds.mesh_id,
             (unsigned)strlen(s_creds.mesh_ap_pass));
    ESP_LOGI(TAG, "MQTT: %s as '%s' (password: %u chars)", s_creds.mqtt_uri,
             s_creds.mqtt_user, (unsigned)strlen(s_creds.mqtt_pass));
    ESP_LOGI(TAG, "OTA URL: %s (token: %u chars)",
             s_creds.ota_url[0] ? s_creds.ota_url : "<unset>",
             (unsigned)strlen(s_creds.ota_token));
}
