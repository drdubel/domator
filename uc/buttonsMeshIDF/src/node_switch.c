/**
 * @file node_switch.c
 * @brief Switch node driver: 7-button input handling and NeoPixel status LED.
 *
 * Runs on ESP32-C3 switch boards.  Provides:
 *  - button_init()  – configure GPIO inputs and install ISR handlers.
 *  - button_task()  – debounce button events and send MSG_TYPE_BUTTON to root.
 *  - led_init()     – configure the single WS2812 LED via the RMT peripheral.
 *  - led_task()     – update the LED colour to reflect mesh connection state.
 *  - led_set_color() / led_flash_cyan() – low-level LED helpers.
 *
 * LED colour semantics:
 *  - Red    – mesh not started.
 *  - Yellow – mesh started, not yet connected.
 *  - Green  – fully connected and operational.
 *  - Cyan   – short flash on button press acknowledgement.
 *  - Blue   – OTA update in progress.
 */

#include <domator_mesh.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
// Button Press Statistics
// ====================

/** @brief Safely increment the global button press counter under the stats
 * mutex. */
static void stats_increment_button_presses(void) {
    if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_stats.button_presses++;
        xSemaphoreGive(g_stats_mutex);
    }
}

// ====================
// Button Initialization
// ====================

/**
 * @brief GPIO ISR handler for switch buttons.
 *        Sets the bit for the triggered button in the task notification value
 *        and wakes button_task().
 * @param arg Button index cast to (void*).
 */
static void IRAM_ATTR button_isr_handler(void* arg) {
    uint32_t button_index = (uint32_t)arg;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    xTaskNotifyFromISR(button_task_handle, (1 << button_index), eSetBits,
                       &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Configure all 7 button GPIOs and install edge-triggered ISR handlers.
 */
void button_init(void) {
    ESP_LOGI(TAG, "Initializing buttons");

    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << g_button_pins[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        gpio_config(&io_conf);

        g_button_states[i].last_state = gpio_get_level(g_button_pins[i]);
        g_button_states[i].last_bounce_time = 0;
        g_button_states[i].press_start_time = 0;
        g_button_states[i].last_release_time = 0;

        ESP_LOGI(TAG, "Button %d initialized on GPIO %d", i, g_button_pins[i]);
    }

    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));

    for (int i = 0; i < NUM_BUTTONS; i++) {
        ESP_ERROR_CHECK(gpio_isr_handler_add(g_button_pins[i],
                                             button_isr_handler, (void*)i));
    }
}

// ====================
// Button Task
// ====================

/**
 * @brief FreeRTOS task: debounce button events and send MSG_TYPE_BUTTON to
 * root.
 */
void button_task(void* arg) {
    ESP_LOGI(TAG, "Button task started");

    uint32_t notified_value;

    while (1) {
        if (g_ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!xTaskNotifyWait(0, 0xFFFFFFFF, &notified_value, portMAX_DELAY)) {
            continue;
        }

        for (int i = 0; i < NUM_BUTTONS; i++) {
            if (!(notified_value & (1 << i))) {
                continue;
            }

            int gpio_num = g_button_pins[i];

            int current_state = gpio_get_level(gpio_num);
            uint32_t current_time = esp_timer_get_time() / 1000;

            if (current_state == g_button_states[i].last_state) {
                continue;
            }

            g_button_states[i].last_state = current_state;

            if (current_time - g_button_states[i].last_bounce_time >
                BUTTON_DEBOUNCE_MS) {
                ESP_LOGI(TAG, "Button %d state changed to %d", i,
                         current_state);

                if (current_state == 0 &&
                    current_time - g_button_states[i].press_start_time >
                        BUTTON_PRESS_OTA_THRESHOLD_MS &&
                    g_button_states[i].last_release_time -
                            g_button_states[i].press_start_time >
                        BUTTON_PRESS_OTA_INTERVAL_MS) {
                    ESP_LOGI(TAG,
                             "Button %d was pressed for %" PRIu32
                             " ms, which exceeds the OTA threshold. "
                             "Triggering OTA...",
                             i,
                             (uint32_t)(current_time -
                                        g_button_states[i].press_start_time));
                    g_ota_requested = true;
                }

                if (current_state == 1) {
                    g_button_states[i].press_start_time = current_time;
                } else {
                    g_button_states[i].last_release_time = current_time;
                }

                stats_increment_button_presses();

                char button_char = 'a' + i;
                mesh_app_msg_t msg = {0};
                msg.src_id = g_device_id;
                msg.msg_type = MSG_TYPE_BUTTON;
                msg.data[0] = button_char;
                msg.data[1] = current_state ? '1' : '0';
                msg.data_len = 2;
                mesh_queue_to_node(&msg, TX_PRIO_NORMAL, NULL);

                ESP_LOGI(TAG,
                         "Sent button '%c' state %d to root. "
                         "Pressed for %" PRIu32 " ms",
                         button_char, current_state,
                         (uint32_t)(current_time -
                                    g_button_states[i].press_start_time));
            }

            g_button_states[i].last_bounce_time = current_time;
        }
    }
}

// ====================
// LED Initialization
// ====================

/**
 * @brief Initialise the WS2812 NeoPixel via the RMT peripheral.
 */
void led_init(void) {
    ESP_LOGI(TAG, "Initializing NeoPixel LED on GPIO %d", LED_GPIO);

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

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

    led_strip_clear(g_led_strip);

    ESP_LOGI(TAG, "NeoPixel LED initialized");
}

// ====================
// LED Set Color
// ====================

/**
 * @brief Set the NeoPixel to the given RGB colour at ~2 % brightness.
 * @param r Red component (0-255 before scaling).
 * @param g Green component (0-255 before scaling).
 * @param b Blue component (0-255 before scaling).
 */
void led_set_color(uint8_t r, uint8_t g, uint8_t b) {
    if (g_led_strip == NULL) {
        return;
    }

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

/**
 * @brief FreeRTOS task: update the NeoPixel colour to reflect mesh state.
 *
 * Also handles OTA blue indication and short cyan flash after a button press.
 */
void led_task(void* arg) {
    ESP_LOGI(TAG, "LED task started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LED_UPDATE_INTERVAL_MS));

        if (g_ota_in_progress) {
            led_set_color(0, 0, 255);
            continue;
        }

        uint32_t current_time = esp_timer_get_time() / 1000;

        if (g_led_flash_active) {
            if (current_time < g_led_flash_end_time) {
                led_set_color(0, 255, 255);
                continue;
            } else {
                g_led_flash_active = false;
            }
        }

        if (g_mesh_connected) {
            led_set_color(0, 255, 0);
        } else if (g_mesh_started) {
            led_set_color(255, 255, 0);
        } else {
            led_set_color(255, 0, 0);
        }
    }
}