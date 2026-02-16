#include <inttypes.h>
#include <string.h>

#include "cJSON.h"
#include "domator_mesh.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

static const char* TAG = "NODE_RELAY";

// Relay initialization flag to prevent operations before init complete
static bool g_relay_initialized = false;

// ====================
// Helper Functions
// ====================

static void stats_increment_button_presses(void) {
    if (xSemaphoreTake(g_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_stats.button_presses++;
        xSemaphoreGive(g_stats_mutex);
    }
}

// ====================
// Board Detection
// ====================

void relay_board_detect(void) {
    // This is called during hardware detection in domator_mesh.c
    // The detection is already done in detect_hardware_type()
    ESP_LOGI(
        TAG, "Board type: %s, relays: %d",
        g_board_type == BOARD_TYPE_16_RELAY ? "16-RELAY" : "8-RELAY",
        g_board_type == BOARD_TYPE_16_RELAY ? MAX_RELAYS_16 : MAX_RELAYS_8);
}

// ====================
// 16-Relay Shift Register Control
// ====================

void relay_write_shift_register(uint16_t bits) {
    if (g_board_type != BOARD_TYPE_16_RELAY) {
        return;
    }

    // Disable outputs while shifting
    gpio_set_level(RELAY_16_PIN_OE, 1);
    gpio_set_level(RELAY_16_PIN_LATCH, 0);

    // Shift out 16 bits, MSB first
    for (int i = 15; i >= 0; i--) {
        gpio_set_level(RELAY_16_PIN_CLOCK, 0);
        gpio_set_level(RELAY_16_PIN_DATA, (bits & (1u << i)) ? 1 : 0);
        gpio_set_level(RELAY_16_PIN_CLOCK, 1);
    }

    // Latch the data and enable outputs
    gpio_set_level(RELAY_16_PIN_LATCH, 1);
    gpio_set_level(RELAY_16_PIN_OE, 0);
}

// ====================
// Relay Control
// ====================

void relay_set(int index, bool state) {
    int max_relays =
        (g_board_type == BOARD_TYPE_16_RELAY) ? MAX_RELAYS_16 : MAX_RELAYS_8;

    if (index < 0 || index >= max_relays) {
        ESP_LOGW(TAG, "Invalid relay index: %d", index);
        return;
    }

    // Safety check: ensure relay is initialized before operations
    if (!g_relay_initialized) {
        ESP_LOGW(TAG, "Relay not initialized, skipping operation");
        return;
    }

    // Safety check: ensure mutex exists
    if (g_relay_mutex == NULL) {
        ESP_LOGE(TAG, "Relay mutex not created, cannot operate relay");
        return;
    }

    if (xSemaphoreTake(g_relay_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire relay mutex");
        return;
    }

    if (g_board_type == BOARD_TYPE_8_RELAY) {
        // Direct GPIO control for 8-relay board
        gpio_set_level(g_relay_8_pins[index], state ? 1 : 0);
        // Update shadow state
        if (state) {
            g_relay_outputs |= (1 << index);
        } else {
            g_relay_outputs &= ~(1 << index);
        }
    } else {
        // Shift register control for 16-relay board
        if (state) {
            g_relay_outputs |= (1 << index);
        } else {
            g_relay_outputs &= ~(1 << index);
        }
        relay_write_shift_register(g_relay_outputs);
    }

    xSemaphoreGive(g_relay_mutex);

    ESP_LOGI(TAG, "Relay %d set to %s", index, state ? "ON" : "OFF");
}

void relay_toggle(int index) {
    int max_relays =
        (g_board_type == BOARD_TYPE_16_RELAY) ? MAX_RELAYS_16 : MAX_RELAYS_8;

    if (index < 0 || index >= max_relays) {
        ESP_LOGW(TAG, "Invalid relay index: %d", index);
        return;
    }

    // Safety check: ensure relay is initialized
    if (!g_relay_initialized) {
        ESP_LOGW(TAG, "Relay not initialized, skipping operation");
        return;
    }

    // Safety check: ensure mutex exists
    if (g_relay_mutex == NULL) {
        ESP_LOGE(TAG, "Relay mutex not created, cannot operate relay");
        return;
    }

    if (xSemaphoreTake(g_relay_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire relay mutex");
        return;
    }

    bool current_state = (g_relay_outputs & (1 << index)) != 0;
    bool new_state = !current_state;

    xSemaphoreGive(g_relay_mutex);

    relay_set(index, new_state);
}

bool relay_get_state(int index) {
    int max_relays =
        (g_board_type == BOARD_TYPE_16_RELAY) ? MAX_RELAYS_16 : MAX_RELAYS_8;

    if (index < 0 || index >= max_relays) {
        return false;
    }

    // Safety check: ensure relay is initialized
    if (!g_relay_initialized) {
        return false;
    }

    // Safety check: ensure mutex exists
    if (g_relay_mutex == NULL) {
        return false;
    }

    if (xSemaphoreTake(g_relay_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    bool state = (g_relay_outputs & (1 << index)) != 0;
    xSemaphoreGive(g_relay_mutex);

    return state;
}

// ====================
// State Sync
// ====================

void relay_send_state_confirmation(int index) {
    bool state = relay_get_state(index);

    // Create relay state message: "A0" means relay 'A' is OFF, "A1" means ON
    mesh_app_msg_t msg = {0};
    msg.msg_type = MSG_TYPE_RELAY_STATE;
    msg.src_id = g_device_id;

    // Format: index + state (e.g., "A0", "A1", "P0", "P1")
    char relay_char = 'A' + index;
    char state_char = state ? '1' : '0';
    msg.data[0] = relay_char;
    msg.data[1] = state_char;
    msg.data[2] = '\0';
    msg.data_len = 2;

    mesh_queue_to_root(&msg, TX_PRIO_NORMAL);
    ESP_LOGD(TAG, "Sent relay state confirmation: %c%c", relay_char,
             state_char);
}

void relay_sync_all_states(void) {
    int max_relays =
        (g_board_type == BOARD_TYPE_16_RELAY) ? MAX_RELAYS_16 : MAX_RELAYS_8;

    ESP_LOGI(TAG, "Syncing all relay states to root");

    for (int i = 0; i < max_relays; i++) {
        relay_send_state_confirmation(i);
    }
}

// ====================
// Command Handling
// ====================

void relay_handle_command(const char* cmd_data) {
    if (cmd_data == NULL || strlen(cmd_data) == 0) {
        ESP_LOGW(TAG, "Empty relay command");
        return;
    }

    // Safety check: ensure relay is initialized before processing commands
    if (!g_relay_initialized) {
        ESP_LOGW(TAG, "Relay not initialized, ignoring command: %s", cmd_data);
        return;
    }

    int max_relays =
        (g_board_type == BOARD_TYPE_16_RELAY) ? MAX_RELAYS_16 : MAX_RELAYS_8;

    // Handle sync request
    if (strcmp(cmd_data, "S") == 0 || strcmp(cmd_data, "sync") == 0) {
        ESP_LOGI(TAG, "Received sync request");
        relay_sync_all_states();
        return;
    }

    // Handle relay command: "a" = toggle relay 0, "a0" = set relay 0 OFF, "a1"
    // = set relay 0 ON
    char relay_char = cmd_data[0];
    int index = -1;

    if (relay_char >= 'a' && relay_char < 'a' + max_relays) {
        index = relay_char - 'a';
    } else if (relay_char >= 'A' && relay_char < 'A' + max_relays) {
        index = relay_char - 'A';
    } else {
        ESP_LOGW(TAG, "Invalid relay character: %c", relay_char);
        return;
    }

    if (strlen(cmd_data) == 1) {
        // Toggle command
        ESP_LOGI(TAG, "Toggle relay %d", index);
        relay_toggle(index);

        // Track button press
        stats_increment_button_presses();
    } else if (strlen(cmd_data) == 2) {
        // Set command
        char state_char = cmd_data[1];
        if (state_char == '0') {
            ESP_LOGI(TAG, "Set relay %d OFF", index);
            relay_set(index, false);
        } else if (state_char == '1') {
            ESP_LOGI(TAG, "Set relay %d ON", index);
            relay_set(index, true);
        } else {
            ESP_LOGW(TAG, "Invalid state character: %c", state_char);
            return;
        }

        // Track button press
        stats_increment_button_presses();
    } else {
        ESP_LOGW(TAG, "Invalid command length: %s", cmd_data);
        return;
    }

    // Send state confirmation
    relay_send_state_confirmation(index);
}

// ====================
// Physical Button Handling
// ====================

static void IRAM_ATTR relay_button_isr_handler(void* arg) {
    uint32_t button_index = (uint32_t)arg;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Notify task, set bit corresponding to button index
    xTaskNotifyFromISR(button_task_handle, (1 << button_index), eSetBits,
                       &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

void relay_button_init(void) {
    ESP_LOGI(TAG, "Initializing relay board buttons");

    // Configure all button pins as input with pull-down
    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << g_relay_button_pins[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        gpio_config(&io_conf);

        // Initialize button states
        g_relay_button_states[i].last_state =
            gpio_get_level(g_relay_button_pins[i]);
        g_relay_button_states[i].last_bounce_time = 0;
        g_relay_button_states[i].press_start_time = 0;
        g_relay_button_states[i].last_release_time = 0;

        ESP_LOGI(TAG, "Relay button %d initialized on GPIO %d", i,
                 g_relay_button_pins[i]);
    }

    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));

    for (int i = 0; i < NUM_BUTTONS; i++) {
        ESP_ERROR_CHECK(gpio_isr_handler_add(
            g_relay_button_pins[i], relay_button_isr_handler, (void*)i));
    }
}

void relay_button_task(void* arg) {
    ESP_LOGI(TAG, "Relay button task started");

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

            int gpio_num = g_relay_button_pins[i];

            int current_state = gpio_get_level(gpio_num);
            uint32_t current_time =
                esp_timer_get_time() / 1000;  // Convert to ms

            if (current_state == g_relay_button_states[i].last_state) {
                continue;
            }

            g_relay_button_states[i].last_state = current_state;

            if (current_time - g_relay_button_states[i].last_bounce_time >
                BUTTON_DEBOUNCE_MS) {
                ESP_LOGI(TAG, "Relay button %d state changed to %d", i,
                         current_state);

                if (current_state == 1) {
                    g_relay_button_states[i].press_start_time = current_time;
                } else {
                    g_relay_button_states[i].last_release_time = current_time;
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
                                    g_relay_button_states[i].press_start_time));
            }

            g_relay_button_states[i].last_bounce_time = current_time;
        }
    }
}

// ====================
// Relay Initialization
// ====================

void relay_init(void) {
    ESP_LOGI(TAG, "Initializing relay board");

    if (g_board_type == BOARD_TYPE_8_RELAY) {
        // Initialize 8 relay output pins
        gpio_config_t io_conf = {0};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

        for (int i = 0; i < MAX_RELAYS_8; i++) {
            io_conf.pin_bit_mask = (1ULL << g_relay_8_pins[i]);
            gpio_config(&io_conf);
            gpio_set_level(g_relay_8_pins[i], 0);  // All relays OFF initially
        }

        // Initialize status LED
        io_conf.pin_bit_mask = (1ULL << RELAY_8_STATUS_LED);
        gpio_config(&io_conf);
        gpio_set_level(RELAY_8_STATUS_LED, 0);  // Status LED OFF initially

        ESP_LOGI(TAG, "8-relay board initialized");
    } else {
        // Initialize shift register pins for 16-relay board
        gpio_config_t io_conf = {0};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pin_bit_mask =
            ((1ULL << RELAY_16_PIN_DATA) | (1ULL << RELAY_16_PIN_CLOCK) |
             (1ULL << RELAY_16_PIN_LATCH) | (1ULL << RELAY_16_PIN_OE));
        gpio_config(&io_conf);

        // Set initial states
        gpio_set_level(RELAY_16_PIN_OE, 0);  // Enable outputs
        gpio_set_level(RELAY_16_PIN_DATA, 0);
        gpio_set_level(RELAY_16_PIN_CLOCK, 0);
        gpio_set_level(RELAY_16_PIN_LATCH, 0);

        // Initialize all relays to OFF
        g_relay_outputs = 0;
        relay_write_shift_register(g_relay_outputs);

        ESP_LOGI(TAG, "16-relay board initialized");
    }

    relay_board_detect();

    // Mark relay as initialized - safe to perform operations now
    g_relay_initialized = true;
    ESP_LOGI(TAG, "Relay initialization complete - ready for operations");
}