#pragma once

/**
 * @file domator_mesh.h
 * @brief Shared types, constants, globals, and function declarations for
 *        the Domator ESP-MESH firmware.
 *
 * This header is included by every compilation unit. It defines:
 *  - Timing and size constants for buttons, LEDs, mesh, and health checks.
 *  - GPIO mappings for all supported hardware boards.
 *  - Wire-level message type codes used between mesh nodes.
 *  - All shared data structures (messages, routing tables, peer health, etc.).
 *  - extern declarations for every global variable.
 *  - Forward declarations for all public functions grouped by source file.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_mesh.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

// Constants for timing, sizes, and limits

#define STATUS_REPORT_INTERVAL_MS 15000
#define BUTTON_POLL_INTERVAL_MS 20
#define BUTTON_DEBOUNCE_MS 15
#define BUTTON_PRESS_TIME_MS 250
#define LED_UPDATE_INTERVAL_MS 100
#define LED_FLASH_DURATION_MS 50
#define LONG_PRESS_THRESHOLD_MS 800
#define ROOT_LOSS_RESET_TIMEOUT_MS 300000    // 5 minutes
#define PEER_HEALTH_CHECK_INTERVAL_MS 30000  // 30 seconds
#define OTA_COUNTDOWN_MS 5000
#define OTA_MAX_FAILURES 3
#define PING_PONG_NUMBER 2

#define NUM_BUTTONS 7
#define MAX_QUEUE_SIZE 30
#define MESH_TX_QUEUE_SIZE 20
#define MESH_MSG_DATA_SIZE 512
#define LOW_HEAP_THRESHOLD 40000
#define CRITICAL_HEAP_THRESHOLD 20000
#define MAX_NODES 64
#define MAX_ROUTES_PER_BUTTON 10
#define MAX_BUTTONS_EXTENDED 24
#define MAX_BUTTONS 8
#define ROUTING_MUTEX_TIMEOUT_MS 200
#define STATS_MUTEX_TIMEOUT_MS 10

// GPIO pin definitions for ESP32-C3 switch board
#define BUTTON_GPIO_0 0
#define BUTTON_GPIO_1 1
#define BUTTON_GPIO_2 3
#define BUTTON_GPIO_3 4
#define BUTTON_GPIO_4 5
#define BUTTON_GPIO_5 6
#define BUTTON_GPIO_6 7
#define LED_GPIO 8

// GPIO pin definitions for 8-relay board
#define RELAY_8_PIN_0 32
#define RELAY_8_PIN_1 33
#define RELAY_8_PIN_2 25
#define RELAY_8_PIN_3 26
#define RELAY_8_PIN_4 27
#define RELAY_8_PIN_5 14
#define RELAY_8_PIN_6 12
#define RELAY_8_PIN_7 13
#define RELAY_8_STATUS_LED 23

// GPIO pin definitions for 8-relay board buttons
#define RELAY_8_BUTTON_0 16
#define RELAY_8_BUTTON_1 17
#define RELAY_8_BUTTON_2 18
#define RELAY_8_BUTTON_3 19
#define RELAY_8_BUTTON_4 21
#define RELAY_8_BUTTON_5 22
#define RELAY_8_BUTTON_6 34
#define RELAY_8_BUTTON_7 35

// GPIO pin definitions for 16-relay shift register board
#define RELAY_16_PIN_DATA 14   // SER
#define RELAY_16_PIN_CLOCK 13  // SRCLK
#define RELAY_16_PIN_LATCH 12  // RCLK
#define RELAY_16_PIN_OE 5      // Output Enable (active LOW)

// Relay board types
#define MAX_RELAYS_8 8
#define MAX_RELAYS_16 16
#define NUM_RELAY_BUTTONS 8

// Message types for mesh communication
#define MSG_TYPE_BUTTON 'B'   // Button press from switch to root
#define MSG_TYPE_STATUS 'S'   // Status update from nodes to root
#define MSG_TYPE_COMMAND 'C'  // Command from root to relay (e.g., toggle relay)
#define MSG_TYPE_ACK 'A'      // Acknowledgment for command receipt
#define MSG_TYPE_RELAY_STATE 'R'   // Relay state confirmation
#define MSG_TYPE_SYNC_REQUEST 'Y'  // Request state sync
#define MSG_TYPE_CONFIG 'G'        // Configuration message
#define MSG_TYPE_TYPE_INFO 'T'     // Message to convey device type info
#define MSG_TYPE_OTA_START 'U'     // OTA update start packet
#define MSG_TYPE_PING 'P'          // Ping message for health check'

// Device types for type info messages
#define DEVICE_TYPE_SWITCH 'S'
#define DEVICE_TYPE_RELAY 'R'

/** @brief Node role/type in the mesh network. */
typedef enum {
    NODE_TYPE_UNKNOWN = 0,
    NODE_TYPE_ROOT,
    NODE_TYPE_SWITCH_C3,
    NODE_TYPE_RELAY_8,
    NODE_TYPE_RELAY_16,
} node_type_t;

/** @brief Hardware board variant. */
typedef enum { BOARD_TYPE_8_RELAY = 0, BOARD_TYPE_16_RELAY } board_type_t;

/** @brief Message transmission priority. */
typedef enum { TX_PRIO_NORMAL = 0, TX_PRIO_HIGH } tx_priority_t;

/** @brief Wire-format application message exchanged between mesh nodes. */
typedef struct {
    uint64_t src_id;
    uint8_t msg_type;
    uint16_t data_len;
    uint32_t data_seq;
    uint8_t target_type;
    char data[MESH_MSG_DATA_SIZE];
} __attribute__((packed)) mesh_app_msg_t;

/** @brief Runtime counters for this device. */
typedef struct {
    uint32_t button_presses;
    uint32_t mesh_send_failed;
    uint32_t mesh_send_success;
    uint32_t mqtt_dropped;
    uint32_t low_heap_events;
    uint32_t critical_heap_events;
    uint32_t mesh_disconnects;
} device_stats_t;

/** @brief Debounce and timing state for a single button. */
typedef struct {
    int last_state;
    uint32_t last_bounce_time;
    uint32_t press_start_time;
    uint32_t last_release_time;
} button_state_t;

/** @brief RGB colour triplet for the NeoPixel status LED. */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

/** @brief One relay target within a button routing entry. */
typedef struct {
    uint64_t target_node_id;
    char relay_command[2];
} route_target_t;

/** @brief All routing targets for a single button. */
typedef struct {
    route_target_t* targets;
    uint8_t num_targets;
} button_route_t;

/** @brief Full routing configuration for one device (all buttons). */
typedef struct {
    button_route_t buttons[MAX_BUTTONS_EXTENDED];
    uint64_t device_id;
} device_connections_t;

/** @brief Per-device button type configuration (toggle=0, stateful=1 per slot).
 */
typedef struct {
    uint64_t device_id;
    uint8_t types[MAX_BUTTONS];
} button_types_t;

/** @brief Runtime health record for a peer node. */
typedef struct {
    uint64_t device_id;
    mesh_addr_t mac_addr;
    uint64_t last_seen;
    uint32_t disconnect_count;
    int8_t last_rssi;
    bool is_alive;
} peer_health_t;

// ====================
// Global Variables (extern)
// ====================

// Device info
extern uint64_t g_device_id;
extern node_type_t g_node_type;
extern char g_firmware_hash[65];
extern device_stats_t g_stats;

// Mesh state
extern volatile bool g_mesh_connected;
extern volatile bool g_mesh_started;
extern volatile bool g_is_root;
extern volatile int g_mesh_layer;
extern uint64_t g_parent_id;

// MQTT (root only)
extern esp_mqtt_client_handle_t g_mqtt_client;
extern volatile bool g_mqtt_connected;

// Routing configuration (root only)
extern device_connections_t g_connections[MAX_NODES];
extern uint8_t g_num_devices;
extern button_types_t g_button_types[MAX_NODES];
extern SemaphoreHandle_t g_connections_mutex;
extern SemaphoreHandle_t g_button_types_mutex;

// Button state (switch nodes)
extern button_state_t g_button_states[NUM_BUTTONS];
extern const int g_button_pins[NUM_BUTTONS];
extern uint32_t g_last_root_contact;

// Relay state (relay nodes)
extern board_type_t g_board_type;
extern uint16_t g_relay_outputs;
extern button_state_t g_relay_button_states[NUM_RELAY_BUTTONS];
extern const int g_relay_8_pins[MAX_RELAYS_8];
extern const int g_relay_button_pins[NUM_RELAY_BUTTONS];
extern SemaphoreHandle_t g_relay_mutex;
extern peer_health_t g_peer_health[MAX_NODES];
extern uint8_t g_peer_count;

// Queues and mutexes
extern QueueHandle_t g_mesh_tx_queue;
extern SemaphoreHandle_t g_stats_mutex;

// Task handles
extern TaskHandle_t button_task_handle;
extern TaskHandle_t telnet_task_handle;

// OTA flag
extern volatile bool g_ota_in_progress;
extern volatile bool g_ota_requested;

// Broadcast address for mesh messages
extern mesh_addr_t g_broadcast_addr;

// ====================
// Function Declarations: mesh_init.c
// ====================

/** @brief Initialise WiFi, the ESP-MESH stack, and register all event handlers.
 */
void mesh_network_init(void);

/** @brief Returns true when the station netif has an IP address. */
bool domator_mesh_is_wifi_connected(void);

/**
 * @brief Stop the mesh stack and connect directly to the router as a plain STA.
 * @param timeout_ms Maximum time in milliseconds to wait for an IP address.
 * @return true on successful connection, false on timeout.
 */
bool mesh_stop_and_connect_sta(uint32_t timeout_ms);

// ====================
// Function Declarations: node_switch.c
// ====================

/** @brief Configure all button GPIOs and install ISR service. */
void button_init(void);

/**
 * @brief FreeRTOS task: handles debounced button events and forwards state
 *        changes to the root node via the mesh TX queue.
 */
void button_task(void* arg);

/** @brief Initialise the NeoPixel LED strip (RMT backend). */
void led_init(void);

/**
 * @brief FreeRTOS task: updates the status LED colour based on mesh state
 *        and handles short flash requests.
 */
void led_task(void* arg);

/**
 * @brief Set the NeoPixel LED to an RGB colour at ~2 % brightness.
 * @param r Red component (0-255).
 * @param g Green component (0-255).
 * @param b Blue component (0-255).
 */
void led_set_color(uint8_t r, uint8_t g, uint8_t b);

/** @brief Trigger a short cyan LED flash to acknowledge a button press. */
void led_flash_cyan(void);

// ====================
// Function Declarations: mesh_comm.c
// ====================

/**
 * @brief FreeRTOS task: receives incoming mesh packets and dispatches them
 *        to the appropriate handler (root or leaf).
 */
void mesh_rx_task(void* arg);

/**
 * @brief FreeRTOS task: drains the internal TX queue and sends each packet
 *        to its destination via the mesh stack.
 */
void mesh_tx_task(void* arg);

/**
 * @brief Enqueue a mesh application message for transmission.
 * @param msg  Pointer to the message to send (copied internally).
 * @param prio Transmission priority (normal or high).
 * @param dest Destination mesh address, or NULL to send to the root node.
 */
void mesh_queue_to_node(mesh_app_msg_t* msg, tx_priority_t prio,
                        mesh_addr_t* dest);

/**
 * @brief FreeRTOS task: periodically publishes a device status report.
 *        Root nodes publish to MQTT; leaf nodes send a JSON status message
 *        over the mesh toward the root.
 */
void status_report_task(void* arg);

// ====================
// Function Declarations: node_root.c
// ====================

/**
 * @brief Dispatch an incoming mesh message received while this node is root.
 * @param from Mesh address of the sender.
 * @param msg  Pointer to the decoded application message.
 */
void root_handle_mesh_message(mesh_addr_t* from, mesh_app_msg_t* msg);

/** @brief Initialise root-only resources (node registry mutex, etc.). */
void node_root_start(void);

/** @brief Initialise and start the MQTT client (root node only). */
void mqtt_init(void);

/** @brief Stop the MQTT client and release all root-specific resources. */
void node_root_stop(void);

/** @brief Build and publish a JSON status report for the root node to MQTT. */
void root_publish_status(void);

// ====================
// Function Declarations: node_relay.c
// ====================

/** @brief Persist the current relay output bitmask to NVS flash. */
void relay_save_states_to_nvs(void);

/** @brief Log the detected board type and relay count. */
void relay_board_detect(void);

/**
 * @brief Configure all relay output GPIOs (or shift-register pins) and
 *        restore the last saved state from NVS.
 */
void relay_init(void);

/**
 * @brief Set a single relay output.
 * @param index Zero-based relay index.
 * @param state true = ON, false = OFF.
 */
void relay_set(int index, bool state);

/**
 * @brief Toggle a single relay output.
 * @param index Zero-based relay index.
 */
void relay_toggle(int index);

/**
 * @brief Shift 16 bits out to the 74HC595 shift-register chain (16-relay
 * board).
 * @param bits Bitmask where bit N controls relay N.
 */
void relay_write_shift_register(uint16_t bits);

/**
 * @brief Return the current on/off state of a relay.
 * @param index Zero-based relay index.
 * @return true if the relay is ON, false otherwise.
 */
bool relay_get_state(int index);

/** @brief Send a MSG_TYPE_RELAY_STATE confirmation for every relay to the root.
 */
void relay_sync_all_states(void);

/**
 * @brief Send a single relay state confirmation message toward the root.
 * @param index Zero-based relay index whose state is being reported.
 */
void relay_send_state_confirmation(int index);

/** @brief Configure relay board button GPIOs and install ISR handlers. */
void relay_button_init(void);

/**
 * @brief FreeRTOS task: handles button interrupts on relay boards and
 *        forwards button state changes to the root node.
 */
void relay_button_task(void* arg);

/**
 * @brief Parse and execute a relay command string.
 *
 * Command format:
 *  - "a"  – toggle relay 0
 *  - "a0" – set relay 0 OFF
 *  - "a1" – set relay 0 ON
 *  - "S" or "sync" – sync all relay states to root
 *
 * @param cmd_data Null-terminated command string.
 */
void relay_handle_command(const char* cmd_data);

// ====================
// Function Declarations: health_ota.c
// ====================

/**
 * @brief FreeRTOS task: monitors g_ota_requested and triggers OTA update
 *        after a short countdown, allowing in-flight messages to drain.
 */
void ota_task(void* arg);

/**
 * @brief Check if OTA has exceeded the maximum failure count and rollback if needed.
 *        Called at boot to catch failed OTA attempts that resulted in a reboot loop.
 */
void ota_check_rollback_on_boot(void);

/**
 * @brief FreeRTOS task: monitors free heap, increments statistics counters
 *        on low/critical heap events, and logs warnings.
 */
void health_monitor_task(void* arg);

// ====================
// Function Declarations: telnet.c
// ====================

/**
 * @brief FreeRTOS task: listens for a Telnet TCP connection on port 23 and
 *        echoes received data back to the client.
 */
void telnet_task(void* arg);

/**
 * @brief Custom vprintf handler that mirrors log output to both UART and
 *        the active Telnet client socket.
 * @param fmt printf-style format string.
 * @param args Variadic argument list.
 * @return Number of bytes written.
 */
int dual_log_vprintf(const char* fmt, va_list args);

/** @brief Start the Telnet server and enable dual UART/Telnet logging. */
void telnet_start(void);

/** @brief Stop the Telnet server and restore the default log handler. */
void telnet_stop(void);