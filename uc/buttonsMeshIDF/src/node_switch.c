#include "domator_mesh.h"
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_timer.h"

static const char *TAG = "NODE_SWITCH";

// LED strip handle
static led_strip_handle_t g_led_strip = NULL;
static led_color_t g_current_led_color = {0, 0, 0};
static bool g_led_flash_active = false;
static uint32_t g_led_flash_end_time = 0;

// ====================
// Button Initialization
// ====================

void button_init(void)
{
    ESP_LOGI(TAG, "Initializing buttons");
    
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
        
        ESP_LOGI(TAG, "Button %d initialized on GPIO %d", i, g_button_pins[i]);
    }
}

// ====================
// Button Task
// ====================

void button_task(void *arg)
{
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
            
            // Check debounce time
            if (current_time - g_button_states[i].last_press_time < BUTTON_DEBOUNCE_MS) {
                continue;
            }
            
            // Detect HIGH transition (button press)
            if (current_state == 1 && g_button_states[i].last_state == 0) {
                g_button_states[i].last_press_time = current_time;
                
                // Increment click counter
                if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    g_stats.button_presses++;
                    xSemaphoreGive(g_stats_mutex);
                }
                
                char button_char = 'a' + i;
                ESP_LOGI(TAG, "Button %d pressed: '%c'", i, button_char);
                
                // Check mesh connection
                if (!g_mesh_connected) {
                    ESP_LOGW(TAG, "Not connected to mesh, cannot send button press");
                    continue;
                }
                
                // Create mesh message
                mesh_app_msg_t msg = {0};
                msg.msg_type = MSG_TYPE_BUTTON;
                msg.device_id = g_device_id;
                msg.data_len = 1;
                msg.data[0] = button_char;
                
                // Queue message to root with high priority
                esp_err_t err = mesh_queue_to_root(&msg);
                if (err == ESP_OK) {
                    ESP_LOGD(TAG, "Button press queued: '%c'", button_char);
                    
                    // Flash LED cyan to confirm
                    led_flash_cyan();
                } else {
                    ESP_LOGW(TAG, "Failed to queue button press");
                }
            }
            
            g_button_states[i].last_state = current_state;
        }
    }
}

// ====================
// LED Initialization
// ====================

void led_init(void)
{
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
    
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &g_led_strip);
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

void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (g_led_strip == NULL) {
        return;
    }
    
    // Reduce brightness (equivalent to Adafruit brightness=5, which is ~2% of 255)
    // Apply brightness factor of 0.02
    r = (r * 5) / 255;
    g = (g * 5) / 255;
    b = (b * 5) / 255;
    
    g_current_led_color.r = r;
    g_current_led_color.g = g;
    g_current_led_color.b = b;
    
    led_strip_set_pixel(g_led_strip, 0, r, g, b);
    led_strip_refresh(g_led_strip);
}

// ====================
// LED Flash Cyan
// ====================

void led_flash_cyan(void)
{
    g_led_flash_active = true;
    g_led_flash_end_time = (esp_timer_get_time() / 1000) + LED_FLASH_DURATION_MS;
}

// ====================
// LED Task
// ====================

void led_task(void *arg)
{
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
