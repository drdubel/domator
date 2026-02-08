#pragma once

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

// ====================
// Constants and Limits
// ====================

#define STATUS_REPORT_INTERVAL_MS 15000
#define BUTTON_POLL_INTERVAL_MS 20
#define BUTTON_DEBOUNCE_MS 250
#define LED_UPDATE_INTERVAL_MS 100
#define LED_FLASH_DURATION_MS 50
#define DOUBLE_PRESS_WINDOW_MS 400
#define LONG_PRESS_THRESHOLD_MS 800
#define ROOT_LOSS_RESET_TIMEOUT_MS 300000    // 5 minutes
#define PEER_HEALTH_CHECK_INTERVAL_MS 30000  // 30 seconds

#define NUM_BUTTONS 7
#define MAX_QUEUE_SIZE 30
#define MESH_TX_QUEUE_SIZE 20
#define MESH_MSG_DATA_SIZE 500
#define LOW_HEAP_THRESHOLD 40000
#define CRITICAL_HEAP_THRESHOLD 20000
#define MAX_DEVICES 50
#define MAX_ROUTES_PER_BUTTON 10
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
#define MSG_TYPE_BUTTON 'B'
#define MSG_TYPE_STATUS 'S'
#define MSG_TYPE_COMMAND 'C'
#define MSG_TYPE_ACK 'A'
#define MSG_TYPE_RELAY_STATE 'R'   // Relay state confirmation
#define MSG_TYPE_SYNC_REQUEST 'Y'  // Request state sync
#define MSG_TYPE_CONFIG 'G'        // Configuration message (gesture config)
#define MSG_TYPE_OTA_TRIGGER 'O'   // OTA update trigger

// Button gesture types
typedef enum {
    GESTURE_NONE = 0,
    GESTURE_SINGLE,
    GESTURE_DOUBLE,
    GESTURE_LONG
} gesture_type_t;

// ============ NODE TYPES ============
typedef enum {
    NODE_TYPE_UNKNOWN = 0,
    NODE_TYPE_ROOT,
    NODE_TYPE_SWITCH_C3,
    NODE_TYPE_RELAY_8,
    NODE_TYPE_RELAY_16,
} node_type_t;

// Relay board types
typedef enum { BOARD_TYPE_8_RELAY = 0, BOARD_TYPE_16_RELAY } board_type_t;

// ====================
// Data Structures
// ====================

// Mesh application message structure
typedef struct {
    uint32_t src_id;
    uint8_t msg_type;
    uint8_t data_len;
    char data[MESH_MSG_DATA_SIZE];
} __attribute__((packed)) mesh_app_msg_t;

// Device statistics
typedef struct {
    uint32_t button_presses;
    uint32_t mesh_send_failed;
    uint32_t mesh_send_success;
    uint32_t mqtt_dropped;
    uint32_t low_heap_events;
    uint32_t critical_heap_events;
    uint32_t mesh_disconnects;
} device_stats_t;

// Button state
typedef struct {
    int last_state;
    uint32_t last_press_time;
    uint32_t press_start_time;
    uint32_t last_release_time;
    bool waiting_for_double;
    gesture_type_t pending_gesture;
} button_state_t;

// LED color
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

// Routing target for button → relay mapping
typedef struct {
    uint32_t target_node_id;
    char relay_command[2];  // e.g., "a", "b1", etc.
} route_target_t;

// Button routing entry
typedef struct {
    route_target_t* targets;
    uint8_t num_targets;
} button_route_t;

// Connection map entry for a device
typedef struct {
    button_route_t buttons[16];  // Support up to 16 buttons (a-p)
} device_connections_t;

// Gesture configuration per button (NVS-persisted)
typedef struct {
    uint8_t
        enabled_gestures;  // Bitmask: bit 0=single, bit 1=double, bit 2=long
} button_gesture_config_t;

// Peer health tracking
typedef struct {
    uint32_t device_id;
    mesh_addr_t mac_addr;  // MAC address for direct routing
    uint32_t last_seen;
    uint32_t disconnect_count;
    int8_t last_rssi;
    bool is_alive;
} peer_health_t;

// ====================
// Global Variables (extern)
// ====================

// Device info
extern uint32_t g_device_id;
extern node_type_t g_node_type;
extern char g_firmware_version[32];
extern char g_firmware_hash[33];
extern device_stats_t g_stats;

// Mesh state
extern bool g_mesh_connected;
extern bool g_mesh_started;
extern bool g_is_root;
extern int g_mesh_layer;
extern uint32_t g_parent_id;

// MQTT (root only)
extern esp_mqtt_client_handle_t g_mqtt_client;
extern bool g_mqtt_connected;

// Routing configuration (root only)
extern device_connections_t g_connections[MAX_DEVICES];
extern uint32_t g_device_ids[MAX_DEVICES];
extern uint8_t g_num_devices;
extern uint8_t g_button_types[MAX_DEVICES][16];  // Button type per device
                                                 // (0=toggle, 1=stateful)
extern SemaphoreHandle_t g_connections_mutex;
extern SemaphoreHandle_t g_button_types_mutex;

// Button state (switch nodes)
extern button_state_t g_button_states[NUM_BUTTONS];
extern const int g_button_pins[NUM_BUTTONS];
extern button_gesture_config_t g_gesture_config[NUM_BUTTONS];
extern uint32_t g_last_root_contact;

// Relay state (relay nodes)
extern board_type_t g_board_type;
extern uint16_t g_relay_outputs;  // 16-bit state for all relays
extern button_state_t g_relay_button_states[NUM_RELAY_BUTTONS];
extern const int g_relay_8_pins[MAX_RELAYS_8];
extern const int g_relay_button_pins[NUM_RELAY_BUTTONS];
extern SemaphoreHandle_t g_relay_mutex;
extern peer_health_t g_peer_health[MAX_DEVICES];
extern uint8_t g_peer_count;

// Queues and mutexes
extern QueueHandle_t g_mesh_tx_queue;
extern SemaphoreHandle_t g_stats_mutex;

// OTA flag
extern bool g_ota_in_progress;

// ============ mesh_init.c ============
void mesh_network_init(void);

// ============ mesh_comm.c ============
void mesh_rx_task(void* arg);
void mesh_tx_task(void* arg);
void mesh_queue_to_root(mesh_app_msg_t* msg);
void mesh_queue_to_node(mesh_addr_t* dest, mesh_app_msg_t* msg);
void status_report_task(void* arg);

// ============ node_root.c ============
void root_handle_mesh_message(mesh_addr_t* from, mesh_app_msg_t* msg);
void node_root_start(void);
void mqtt_init(void);
void node_root_stop(void);
void root_publish_status(void);