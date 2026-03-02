/**
 * @file node_relay.c
 * @brief Relay board driver and button handling for 8-relay and 16-relay nodes.
 *
 * Supports two hardware variants:
 *  - BOARD_TYPE_8_RELAY  – 8 direct-GPIO relay outputs + 8 physical buttons.
 *  - BOARD_TYPE_16_RELAY – 16-output 74HC595 shift-register chain.
 *
 * Responsibilities:
 *  - Initialise relay outputs and restore persisted state from NVS on boot.
 *  - Provide relay_set() / relay_toggle() with mutex protection.
 *  - Send state confirmation messages to the root after every change.
 *  - Handle relay command strings arriving from the mesh (root → relay).
 *  - Detect and debounce physical buttons mounted on relay boards.
 */

#include <inttypes.h>
#include <string.h>

#include "cJSON.h"
#include "domator_mesh.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

static const char* TAG = "NODE_RELAY";

/** @brief Initialization guard: set to true once hardware setup is complete. */
static bool g_relay_initialized = false;

// ====================
// Helper Functions
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
// NVS Flash Storage for Relay States
// ====================

/**
 * @brief Persist g_relay_outputs to NVS so relay states survive a reboot.
 *        Writes a 16-bit value under the "relay_states" namespace.
 */
void relay_save_states_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("relay_states", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for relay states: %s",
                 esp_err_to_name(err));
        return;
    }

    uint16_t previous_outputs = 0;
    err = nvs_get_u16(nvs_handle, "outputs", &previous_outputs);
    if (err == ESP_OK) {
        if (previous_outputs == g_relay_outputs) {
            ESP_LOGI(TAG, "Relay states unchanged, no need to update NVS");
            nvs_close(nvs_handle);
            return;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to read previous relay states from NVS: %s",
                 esp_err_to_name(err));
        nvs_close(nvs_handle);
        return;
    }

    err = nvs_set_u16(nvs_handle, "outputs", g_relay_outputs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save relay states to NVS: %s",
                 esp_err_to_name(err));
    } else {
        nvs_commit(nvs_handle);
        ESP_LOGI(TAG, "Relay states saved to NVS");
    }

    nvs_close(nvs_handle);
}

/**
 * @brief Load relay states from NVS and apply them to the hardware.
 *        If no saved state exists, all relays remain OFF.
 */
void relay_load_states_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("relay_states", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved relay states in NVS");
        return;
    }

    uint16_t saved_outputs = 0;
    err = nvs_get_u16(nvs_handle, "outputs", &saved_outputs);
    if (err == ESP_OK) {
        g_relay_outputs = saved_outputs;
        ESP_LOGI(TAG, "Loaded relay states from NVS: 0x%04X", g_relay_outputs);
        if (g_board_type == BOARD_TYPE_8_RELAY) {
            for (int i = 0; i < MAX_RELAYS_8; i++) {
                bool state = (g_relay_outputs & (1 << i)) != 0;
                relay_set(i, state);
            }
        } else {
            for (int i = 0; i < MAX_RELAYS_16; i++) {
                bool state = (g_relay_outputs & (1 << i)) != 0;
                relay_set(i, state);
            }
        }
    } else {
        ESP_LOGW(TAG, "Failed to read relay states from NVS: %s",
                 esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
}

// ====================
// Board Detection
// ====================

/**
 * @brief Log the detected board type and relay count.
 */
void relay_board_detect(void) {
    ESP_LOGI(
        TAG, "Board type: %s, relays: %d",
        g_board_type == BOARD_TYPE_16_RELAY ? "16-RELAY" : "8-RELAY",
        g_board_type == BOARD_TYPE_16_RELAY ? MAX_RELAYS_16 : MAX_RELAYS_8);
}

// ====================
// 16-Relay Shift Register Control
// ====================

/**
 * @brief Shift 16 bits out to the 74HC595 chain, MSB first, then latch.
 * @param bits Bitmask where bit N controls relay N.
 */
void relay_write_shift_register(uint16_t bits) {
    if (g_board_type != BOARD_TYPE_16_RELAY) {
        return;
    }

    gpio_set_level(RELAY_16_PIN_OE, 1);
    gpio_set_level(RELAY_16_PIN_LATCH, 0);

    for (int i = 15; i >= 0; i--) {
        gpio_set_level(RELAY_16_PIN_CLOCK, 0);
        gpio_set_level(RELAY_16_PIN_DATA, (bits & (1u << i)) ? 1 : 0);
        gpio_set_level(RELAY_16_PIN_CLOCK, 1);
    }

    gpio_set_level(RELAY_16_PIN_LATCH, 1);
    gpio_set_level(RELAY_16_PIN_OE, 0);
}

// ====================
// Relay Control
// ====================

/**
 * @brief Set a single relay output to the given state.
 * @param index Zero-based relay index.
 * @param state true = ON, false = OFF.
 */
void relay_set(int index, bool state) {
    int max_relays =
        (g_board_type == BOARD_TYPE_16_RELAY) ? MAX_RELAYS_16 : MAX_RELAYS_8;

    if (index < 0 || index >= max_relays) {
        ESP_LOGW(TAG, "Invalid relay index: %d", index);
        return;
    }

    if (!g_relay_initialized) {
        ESP_LOGW(TAG, "Relay not initialized, skipping operation");
        return;
    }

    if (g_relay_mutex == NULL) {
        ESP_LOGE(TAG, "Relay mutex not created, cannot operate relay");
        return;
    }

    if (xSemaphoreTake(g_relay_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire relay mutex");
        return;
    }

    if (g_board_type == BOARD_TYPE_8_RELAY) {
        gpio_set_level(g_relay_8_pins[index], state ? 1 : 0);
        if (state) {
            g_relay_outputs |= (1 << index);
        } else {
            g_relay_outputs &= ~(1 << index);
        }
    } else {
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

/**
 * @brief Toggle a single relay output.
 * @param index Zero-based relay index.
 */
void relay_toggle(int index) {
    int max_relays =
        (g_board_type == BOARD_TYPE_16_RELAY) ? MAX_RELAYS_16 : MAX_RELAYS_8;

    if (index < 0 || index >= max_relays) {
        ESP_LOGW(TAG, "Invalid relay index: %d", index);
        return;
    }

    if (!g_relay_initialized) {
        ESP_LOGW(TAG, "Relay not initialized, skipping operation");
        return;
    }

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

/**
 * @brief Return the current on/off state of a relay.
 * @param index Zero-based relay index.
 * @return true if ON, false if OFF or on error.
 */
bool relay_get_state(int index) {
    int max_relays =
        (g_board_type == BOARD_TYPE_16_RELAY) ? MAX_RELAYS_16 : MAX_RELAYS_8;

    if (index < 0 || index >= max_relays) {
        return false;
    }

    if (!g_relay_initialized) {
        return false;
    }

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

/**
 * @brief Send a MSG_TYPE_RELAY_STATE confirmation for a single relay to root.
 * @param index Zero-based relay index.
 */
void relay_send_state_confirmation(int index) {
    bool state = relay_get_state(index);

    mesh_app_msg_t msg = {0};
    msg.msg_type = MSG_TYPE_RELAY_STATE;
    msg.src_id = g_device_id;

    char relay_char = 'A' + index;
    char state_char = state ? '1' : '0';
    msg.data[0] = relay_char;
    msg.data[1] = state_char;
    msg.data[2] = '\0';
    msg.data_len = 2;

    mesh_queue_to_node(&msg, TX_PRIO_NORMAL, NULL);
    ESP_LOGD(TAG, "Sent relay state confirmation: %c%c", relay_char,
             state_char);
}

/**
 * @brief Send MSG_TYPE_RELAY_STATE confirmations for all relays to the root.
 */
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

/**
 * @brief Parse and execute a relay command string.
 *
 * Supported formats:
 *  - "a"      – toggle relay 0
 *  - "a0"     – set relay 0 OFF
 *  - "a1"     – set relay 0 ON
 *  - "S"/"sync" – sync all states to root
 *
 * @param cmd_data Null-terminated command string.
 */
void relay_handle_command(const char* cmd_data) {
    if (cmd_data == NULL || strlen(cmd_data) == 0) {
        ESP_LOGW(TAG, "Empty relay command");
        return;
    }

    if (!g_relay_initialized) {
        ESP_LOGW(TAG, "Relay not initialized, ignoring command: %s", cmd_data);
        return;
    }

    int max_relays =
        (g_board_type == BOARD_TYPE_16_RELAY) ? MAX_RELAYS_16 : MAX_RELAYS_8;

    if (strcmp(cmd_data, "S") == 0 || strcmp(cmd_data, "sync") == 0) {
        ESP_LOGI(TAG, "Received sync request");
        relay_sync_all_states();
        return;
    }

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
        ESP_LOGI(TAG, "Toggle relay %d", index);
        relay_toggle(index);
        stats_increment_button_presses();
    } else if (strlen(cmd_data) == 2) {
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
        stats_increment_button_presses();
    } else {
        ESP_LOGW(TAG, "Invalid command length: %s", cmd_data);
        return;
    }

    relay_send_state_confirmation(index);
}

// ====================
// Physical Button ISR
// ====================

/**
 * @brief GPIO ISR handler for relay board buttons.
 *        Notifies relay_button_task() by setting the bit corresponding to the
 *        button index in the task notification value.
 * @param arg Button index cast to (void*).
 */
static void IRAM_ATTR relay_button_isr_handler(void* arg) {
    uint32_t button_index = (uint32_t)arg;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    xTaskNotifyFromISR(button_task_handle, (1 << button_index), eSetBits,
                       &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Configure relay board button GPIOs and install ISR handlers.
 */
void relay_button_init(void) {
    ESP_LOGI(TAG, "Initializing relay board buttons");

    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << g_relay_button_pins[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        gpio_config(&io_conf);

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

/**
 * @brief FreeRTOS task: handle button interrupts on relay boards and forward
 *        button state changes to the root node via MSG_TYPE_BUTTON.
 */
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
            uint32_t current_time = esp_timer_get_time() / 1000;

            if (current_state == g_relay_button_states[i].last_state) {
                continue;
            }

            g_relay_button_states[i].last_state = current_state;

            if (current_time - g_relay_button_states[i].last_bounce_time >
                BUTTON_DEBOUNCE_MS) {
                ESP_LOGI(TAG, "Relay button %d state changed to %d", i,
                         current_state);

                if (current_state == 0 &&
                    current_time - g_relay_button_states[i].press_start_time >
                        BUTTON_PRESS_OTA_THRESHOLD_MS &&
                    g_relay_button_states[i].press_start_time -
                            g_relay_button_states[i].last_release_time <
                        BUTTON_PRESS_OTA_INTERVAL_MS) {
                    ESP_LOGI(
                        TAG,
                        "Button %d was pressed for %" PRIu32
                        " ms, which exceeds the OTA threshold. "
                        "Triggering OTA...",
                        i,
                        (uint32_t)(current_time -
                                   g_relay_button_states[i].press_start_time));
                    g_ota_requested = true;
                }

                if (current_state == 1) {
                    g_relay_button_states[i].press_start_time = current_time;
                } else {
                    g_relay_button_states[i].last_release_time = current_time;
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
                                    g_relay_button_states[i].press_start_time));
            }

            g_relay_button_states[i].last_bounce_time = current_time;
        }
    }
}

// ====================
// Relay Initialization
// ====================

/**
 * @brief Configure all relay output GPIOs (or shift-register pins) and
 *        restore the last saved state from NVS.
 */
void relay_init(void) {
    ESP_LOGI(TAG, "Initializing relay board");

    if (g_board_type == BOARD_TYPE_8_RELAY) {
        gpio_config_t io_conf = {0};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

        for (int i = 0; i < MAX_RELAYS_8; i++) {
            io_conf.pin_bit_mask = (1ULL << g_relay_8_pins[i]);
            gpio_config(&io_conf);
            gpio_set_level(g_relay_8_pins[i], 0);
        }

        io_conf.pin_bit_mask = (1ULL << RELAY_8_STATUS_LED);
        gpio_config(&io_conf);
        gpio_set_level(RELAY_8_STATUS_LED, 0);

        ESP_LOGI(TAG, "8-relay board initialized");
    } else {
        gpio_config_t io_conf = {0};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pin_bit_mask =
            ((1ULL << RELAY_16_PIN_DATA) | (1ULL << RELAY_16_PIN_CLOCK) |
             (1ULL << RELAY_16_PIN_LATCH) | (1ULL << RELAY_16_PIN_OE));
        gpio_config(&io_conf);

        gpio_set_level(RELAY_16_PIN_OE, 0);
        gpio_set_level(RELAY_16_PIN_DATA, 0);
        gpio_set_level(RELAY_16_PIN_CLOCK, 0);
        gpio_set_level(RELAY_16_PIN_LATCH, 0);

        g_relay_outputs = 0;
        relay_write_shift_register(g_relay_outputs);

        ESP_LOGI(TAG, "16-relay board initialized");
    }

    relay_board_detect();

    g_relay_initialized = true;

    ESP_LOGI(TAG, "Relay initialization complete - ready for operations");

    relay_load_states_from_nvs();
}