#include <domator_mesh.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "NODE_SWITCH";

// LED strip handle
static led_strip_handle_t g_led_strip = NULL;
static led_color_t g_current_led_color = {0, 0, 0};
static bool g_led_flash_active = false;
static uint32_t g_led_flash_end_time = 0;

// ====================
// Gesture Configuration
// ====================

// Convert gesture type and button index to character
// Single: 'a'-'g', Double: 'h'-'n', Long: 'o'-'u'
char gesture_to_char(int button_index, gesture_type_t gesture) {
    if (button_index < 0 || button_index >= NUM_BUTTONS) {
        return '\0';
    }

    switch (gesture) {
        case GESTURE_SINGLE:
            return 'a' + button_index;  // a-h
        case GESTURE_DOUBLE:
            return 'i' + button_index;  // i-o
        case GESTURE_LONG:
            return 'p' + button_index;  // p-v
        default:
            return '\0';
    }
}

// Check if a gesture is enabled for a button
bool is_gesture_enabled(int button_index, gesture_type_t gesture) {
    if (button_index < 0 || button_index >= NUM_BUTTONS) {
        return false;
    }

    uint8_t mask = g_gesture_config[button_index].enabled_gestures;

    switch (gesture) {
        case GESTURE_SINGLE:
            return (mask & 0x01) != 0;  // bit 0
        case GESTURE_DOUBLE:
            return (mask & 0x02) != 0;  // bit 1
        case GESTURE_LONG:
            return (mask & 0x04) != 0;  // bit 2
        default:
            return false;
    }
}

// Load gesture configuration from NVS
void gesture_config_load(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("domator", NVS_READONLY, &nvs_handle);

    if (err == ESP_OK) {
        for (int i = 0; i < NUM_BUTTONS; i++) {
            char key[16];
            snprintf(key, sizeof(key), "gesture_%d", i);

            uint8_t value = 0x07;  // Default: all gestures enabled (bits 0,1,2)
            err = nvs_get_u8(nvs_handle, key, &value);

            if (err == ESP_OK) {
                g_gesture_config[i].enabled_gestures = value;
                ESP_LOGI(TAG, "Loaded gesture config for button %d: 0x%02X", i,
                         value);
            } else if (err == ESP_ERR_NVS_NOT_FOUND) {
                // Not found - use default (all enabled)
                g_gesture_config[i].enabled_gestures = 0x07;
                ESP_LOGI(TAG,
                         "No gesture config for button %d, using default (all "
                         "enabled)",
                         i);
            } else {
                ESP_LOGW(TAG, "Error loading gesture config for button %d: %s",
                         i, esp_err_to_name(err));
                g_gesture_config[i].enabled_gestures = 0x07;
            }
        }
        nvs_close(nvs_handle);
    } else {
        // NVS not available or error - default to all gestures enabled
        ESP_LOGW(TAG, "Failed to open NVS for gesture config: %s",
                 esp_err_to_name(err));
        for (int i = 0; i < NUM_BUTTONS; i++) {
            g_gesture_config[i].enabled_gestures = 0x07;
        }
    }

    ESP_LOGI(TAG, "Gesture configuration loaded");
}

// Save gesture configuration to NVS
void gesture_config_save(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("domator", NVS_READWRITE, &nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for saving gesture config: %s",
                 esp_err_to_name(err));
        return;
    }

    for (int i = 0; i < NUM_BUTTONS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "gesture_%d", i);

        err = nvs_set_u8(nvs_handle, key, g_gesture_config[i].enabled_gestures);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save gesture config for button %d: %s", i,
                     esp_err_to_name(err));
        }
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Gesture configuration saved to NVS");
}

// ====================
// Button Initialization
// ====================

void button_init(void) {
    ESP_LOGI(TAG, "Initializing buttons");

    // Load gesture configuration from NVS
    gesture_config_load();

    // Configure all button pins as input with pull-down
    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << g_button_pins[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);

        // Initialize button states
        g_button_states[i].last_state = gpio_get_level(g_button_pins[i]);
        g_button_states[i].last_press_time = 0;
        g_button_states[i].press_start_time = 0;
        g_button_states[i].last_release_time = 0;
        g_button_states[i].waiting_for_double = false;
        g_button_states[i].pending_gesture = GESTURE_NONE;

        ESP_LOGI(TAG, "Button %d initialized on GPIO %d, gestures: 0x%02X", i,
                 g_button_pins[i], g_gesture_config[i].enabled_gestures);
    }
}

// ====================
// Button Task
// ====================

void button_task(void* arg) {
    ESP_LOGI(TAG, "Button task started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_INTERVAL_MS));

        if (g_ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        uint32_t current_time = esp_timer_get_time() / 1000;  // Convert to ms

        for (int i = 0; i < NUM_BUTTONS; i++) {
            int current_state = gpio_get_level(g_button_pins[i]);
            button_state_t* btn = &g_button_states[i];

            // Detect state transitions
            if (current_state != btn->last_state) {
                // Debounce check
                if (current_time - btn->last_press_time < BUTTON_DEBOUNCE_MS) {
                    continue;
                }

                if (current_state == 1) {
                    // Button pressed (LOW → HIGH transition)
                    btn->press_start_time = current_time;
                    btn->last_press_time = current_time;

                    // Check if this is a double press
                    if (btn->waiting_for_double &&
                        (current_time - btn->last_release_time) <
                            DOUBLE_PRESS_WINDOW_MS) {
                        // Double press detected!
                        btn->waiting_for_double = false;
                        btn->pending_gesture = GESTURE_DOUBLE;

                        ESP_LOGI(TAG, "Button %d: Double press detected", i);

                        // Send immediately if double gesture is enabled
                        if (is_gesture_enabled(i, GESTURE_DOUBLE)) {
                            // Increment counter
                            if (xSemaphoreTake(g_stats_mutex,
                                               pdMS_TO_TICKS(100)) == pdTRUE) {
                                g_stats.button_presses++;
                                xSemaphoreGive(g_stats_mutex);
                            }

                            char button_char =
                                gesture_to_char(i, GESTURE_DOUBLE);
                            ESP_LOGI(TAG, "Sending double press: '%c'",
                                     button_char);

                            if (g_mesh_connected) {
                                mesh_app_msg_t msg = {0};
                                msg.msg_type = MSG_TYPE_BUTTON;
                                msg.src_id = g_device_id;
                                msg.data_len = 1;
                                msg.data[0] = button_char;

                                mesh_queue_to_root(&msg);
                                led_flash_cyan();
                            }
                        } else {
                            // Double disabled, fall back to single
                            ESP_LOGI(
                                TAG,
                                "Double press disabled, sending single press");
                            char button_char =
                                gesture_to_char(i, GESTURE_SINGLE);

                            if (g_mesh_connected) {
                                mesh_app_msg_t msg = {0};
                                msg.msg_type = MSG_TYPE_BUTTON;
                                msg.src_id = g_device_id;
                                msg.data_len = 1;
                                msg.data[0] = button_char;

                                mesh_queue_to_root(&msg);
                                led_flash_cyan();
                            }
                        }
                    } else {
                        // First press - wait to see if it's single, double, or
                        // long
                        btn->pending_gesture = GESTURE_SINGLE;
                    }
                } else {
                    // Button released (HIGH → LOW transition)
                    btn->last_release_time = current_time;
                    uint32_t press_duration =
                        current_time - btn->press_start_time;

                    // Check if it was a long press
                    if (press_duration >= LONG_PRESS_THRESHOLD_MS &&
                        btn->pending_gesture != GESTURE_DOUBLE) {
                        // Long press detected
                        btn->pending_gesture = GESTURE_LONG;
                        btn->waiting_for_double = false;

                        ESP_LOGI(TAG, "Button %d: Long press detected (%lu ms)",
                                 i, press_duration);

                        if (is_gesture_enabled(i, GESTURE_LONG)) {
                            // Increment counter
                            if (xSemaphoreTake(g_stats_mutex,
                                               pdMS_TO_TICKS(100)) == pdTRUE) {
                                g_stats.button_presses++;
                                xSemaphoreGive(g_stats_mutex);
                            }

                            char button_char = gesture_to_char(i, GESTURE_LONG);
                            ESP_LOGI(TAG, "Sending long press: '%c'",
                                     button_char);

                            if (g_mesh_connected) {
                                mesh_app_msg_t msg = {0};
                                msg.msg_type = MSG_TYPE_BUTTON;
                                msg.src_id = g_device_id;
                                msg.data_len = 1;
                                msg.data[0] = button_char;

                                mesh_queue_to_root(&msg);
                                led_flash_cyan();
                            }
                        } else {
                            // Long disabled, fall back to single
                            ESP_LOGI(
                                TAG,
                                "Long press disabled, sending single press");
                            char button_char =
                                gesture_to_char(i, GESTURE_SINGLE);

                            if (g_mesh_connected) {
                                mesh_app_msg_t msg = {0};
                                msg.msg_type = MSG_TYPE_BUTTON;
                                msg.src_id = g_device_id;
                                msg.data_len = 1;
                                msg.data[0] = button_char;
                                mesh_queue_to_root(&msg);
                                led_flash_cyan();
                            }
                        }
                    } else if (btn->pending_gesture == GESTURE_SINGLE &&
                               btn->pending_gesture != GESTURE_DOUBLE) {
                        // Short press - wait for possible double press
                        btn->waiting_for_double = true;
                        ESP_LOGD(TAG,
                                 "Button %d: Waiting for possible double press",
                                 i);
                    }
                }

                btn->last_state = current_state;
            }

            // Check for double press timeout
            if (btn->waiting_for_double/* &&
                (current_time - btn->last_release_time) >
                    DOUBLE_PRESS_WINDOW_MS */) {
                // Double press window expired - send single press
                btn->waiting_for_double = false;

                ESP_LOGI(TAG, "Button %d: Single press confirmed", i);

                if (is_gesture_enabled(i, GESTURE_SINGLE)) {
                    // Increment counter
                    if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) ==
                        pdTRUE) {
                        g_stats.button_presses++;
                        xSemaphoreGive(g_stats_mutex);
                    }

                    char button_char = gesture_to_char(i, GESTURE_SINGLE);
                    ESP_LOGI(TAG, "Sending single press: '%c'", button_char);

                    if (g_mesh_connected) {
                        mesh_app_msg_t msg = {0};
                        msg.msg_type = MSG_TYPE_BUTTON;
                        msg.src_id = g_device_id;
                        msg.data_len = 1;
                        msg.data[0] = button_char;

                        mesh_queue_to_root(&msg);
                        led_flash_cyan();
                    } else {
                        ESP_LOGW(
                            TAG,
                            "Not connected to mesh, cannot send button press");
                    }
                } else {
                    // Single disabled but this shouldn't happen (fallback
                    // default)
                    ESP_LOGW(TAG, "Single press disabled for button %d", i);
                }
            }
        }
    }
}

// ====================
// LED Initialization
// ====================

void led_init(void) {
    ESP_LOGI(TAG, "Initializing NeoPixel LED on GPIO %d", LED_GPIO);

    // LED strip configuration for ESP-IDF 5.x
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    // RMT backend configuration
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10MHz
        .flags.with_dma = false,
    };

    esp_err_t ret =
        led_strip_new_rmt_device(&strip_config, &rmt_config, &g_led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(ret));
        return;
    }

    // Clear LED
    led_strip_clear(g_led_strip);

    ESP_LOGI(TAG, "NeoPixel LED initialized");
}

// ====================
// LED Set Color
// ====================

void led_set_color(uint8_t r, uint8_t g, uint8_t b) {
    if (g_led_strip == NULL) {
        return;
    }

    // Reduce brightness to ~2% (divide by 51 to achieve 1/51 = ~2% brightness)
    // This is equivalent to Adafruit NeoPixel brightness setting of 5/255
    r = r / 51;
    g = g / 51;
    b = b / 51;

    g_current_led_color.r = r;
    g_current_led_color.g = g;
    g_current_led_color.b = b;

    led_strip_set_pixel(g_led_strip, 0, r, g, b);
    led_strip_refresh(g_led_strip);
}

// ====================
// LED Flash Cyan
// ====================

void led_flash_cyan(void) {
    g_led_flash_active = true;
    g_led_flash_end_time =
        (esp_timer_get_time() / 1000) + LED_FLASH_DURATION_MS;
}

// ====================
// LED Task
// ====================

void led_task(void* arg) {
    ESP_LOGI(TAG, "LED task started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LED_UPDATE_INTERVAL_MS));

        if (g_ota_in_progress) {
            // Blue during OTA
            led_set_color(0, 0, 255);
            continue;
        }

        uint32_t current_time = esp_timer_get_time() / 1000;  // Convert to ms

        // Check if flash is active
        if (g_led_flash_active) {
            if (current_time < g_led_flash_end_time) {
                // Flash cyan
                led_set_color(0, 255, 255);
                continue;
            } else {
                g_led_flash_active = false;
            }
        }

        // Normal status indication
        if (g_mesh_connected) {
            // Green - fully connected and operational
            led_set_color(0, 255, 0);
        } else if (g_mesh_started) {
            // Yellow - mesh started but not connected
            led_set_color(255, 255, 0);
        } else {
            // Red - not connected to mesh
            led_set_color(255, 0, 0);
        }
    }
}
