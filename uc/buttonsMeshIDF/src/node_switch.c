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

// ==================== Button Press Statistics ====================
static void stats_increment_button_presses(void) {
    if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_stats.button_presses++;
        xSemaphoreGive(g_stats_mutex);
    }
}

// ==================== Button Initialization ====================
static void IRAM_ATTR button_isr_handler(void* arg) {
    uint32_t button_index = (uint32_t)arg;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Notify task, set bit corresponding to button index
    xTaskNotifyFromISR(button_task_handle, (1 << button_index), eSetBits,
                       &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

void button_init(void) {
    ESP_LOGI(TAG, "Initializing buttons");

    // Configure all button pins as input with pull-down
    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << g_button_pins[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        gpio_config(&io_conf);

        // Initialize button states
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
            uint32_t current_time =
                esp_timer_get_time() / 1000;  // Convert to ms

            if (current_state == g_button_states[i].last_state) {
                continue;
            }

            g_button_states[i].last_state = current_state;

            if (current_time - g_button_states[i].last_bounce_time >
                BUTTON_DEBOUNCE_MS) {
                ESP_LOGI(TAG, "Button %d state changed to %d", i,
                         current_state);

                if (current_state == 1) {
                    g_button_states[i].press_start_time = current_time;
                } else {
                    g_button_states[i].last_release_time = current_time;
                }

                stats_increment_button_presses();

                // Send button press message to root
                char button_char = 'a' + i;  // 'a'-'g'
                mesh_app_msg_t msg = {0};
                msg.src_id = g_device_id;
                msg.msg_type = MSG_TYPE_BUTTON;
                msg.data[0] = button_char;
                msg.data[1] = current_state + '0';  // '0' or '1'
                msg.data_len = 2;
                mesh_queue_to_root(&msg, TX_PRIO_NORMAL);

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

    // Reduce brightness to ~2% (divide by 51 to achieve 1/51 = ~2%
    // brightness) This is equivalent to Adafruit NeoPixel brightness
    // setting of 5/255
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
