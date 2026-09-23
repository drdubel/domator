#pragma once

/**
 * @file credentials.h
 * @brief Per-installation settings, read from NVS instead of the binary.
 *
 * Nothing here is compiled into the firmware. Every value lives in the
 * "domator" NVS namespace and is written over the wire at programming time
 * (see provisioning/README.md), for two reasons:
 *
 *  - A firmware image with credentials in it leaks them to anyone who obtains
 *    the binary -- `strings firmware.bin` is enough. That happened once
 *    already, via a publicly readable OTA endpoint.
 *  - With the settings out of the image, one build runs on every
 *    installation, so an OTA image is no longer household-specific.
 *
 * credentials_load() must run once, early in app_main(), before WiFi, the mesh
 * or MQTT are touched. There are no compiled-in fallbacks: if NVS holds no
 * credentials the device refuses to join anything.
 */

#include <stdbool.h>

#include "esp_err.h"

/**
 * Dedicated NVS partition (see partitions.csv), separate from the default
 * "nvs" one so that the erase-on-corruption path in app_main(), and any OTA
 * tooling that rewrites nvs, cannot take the credentials with them.
 */
#define DOMATOR_NVS_PARTITION "creds"

/** NVS namespace holding every value below. */
#define DOMATOR_NVS_NAMESPACE "domator"

/** Mesh IDs are exactly 6 bytes on the wire. */
#define DOMATOR_MESH_ID_LEN 6

typedef struct {
    char router_ssid[33];    // 32 + NUL
    char router_pass[65];    // WPA2 max 63, + NUL, + slack
    char mesh_id[33];  // only the first DOMATOR_MESH_ID_LEN bytes are used
    char mesh_ap_pass[65];
    char mqtt_uri[128];
    char mqtt_user[33];
    char mqtt_pass[65];
    char ota_url[192];
    char ota_token[129];
} domator_credentials_t;

/**
 * @brief Read every setting from the "domator" NVS namespace into RAM.
 *
 * @return ESP_OK when all required keys were present and well-formed.
 *         ESP_ERR_NVS_NOT_FOUND when the namespace or a required key is
 *         missing -- the device has not been provisioned yet.
 */
esp_err_t credentials_load(void);

/** @brief True once credentials_load() has succeeded. */
bool credentials_are_loaded(void);

/**
 * @brief The loaded settings. Never NULL, but all-empty until
 *        credentials_load() succeeds -- callers run after it by construction.
 */
const domator_credentials_t* credentials_get(void);

/** @brief Log what was loaded, with the secret values masked. */
void credentials_log_summary(void);
