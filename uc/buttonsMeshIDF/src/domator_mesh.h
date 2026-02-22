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

<<<<<<< HEAD
// ====================
// Constants and Limits
// ====================

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
#define PING_PONG_NUMBER 50
#define PING_PONG_NUMBER 50

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
    NODE_TYPE_UNKNOWN = 0, NODE_TYPE_ROOT,
=======
    // ============ NODE TYPES ============
    typedef enum {
        NODE_TYPE_UNKNOWN = 0,
>>>>>>> 75e1902 (changed to my version)
        NODE_TYPE_SWITCH_C3,
        NODE_TYPE_RELAY_8,
        NODE_TYPE_RELAY_16,
    } node_type_t;

<<<<<<< HEAD
    // Relay board types
    typedef enum {BOARD_TYPE_8_RELAY = 0, BOARD_TYPE_16_RELAY} board_type_t;

    // Transmission priority levels
    typedef enum {TX_PRIO_NORMAL = 0, TX_PRIO_HIGH} tx_priority_t;

    // ====================
    // Data Structures
    // ====================

    // Mesh application message structure
    typedef struct {uint64_t src_id; uint8_t msg_type; uint16_t data_len;
                    uint32_t data_seq; uint8_t target_type;
                    char data[MESH_MSG_DATA_SIZE];}
__attribute__((packed)) mesh_app_msg_t;

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
    uint32_t last_bounce_time;
    uint32_t press_start_time;
    uint32_t last_release_time;
} button_state_t;

// LED color
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

// Routing target for button → relay mapping
typedef struct {
    uint64_t target_node_id;
    char relay_command[2];
} route_target_t;

// Button routing entry
typedef struct {
    route_target_t* targets;
    uint8_t num_targets;
} button_route_t;

// Connection map entry for a device
typedef struct {
    button_route_t
        buttons[MAX_BUTTONS_EXTENDED];  // Support up to 24 buttons (a-x)
    uint64_t device_id;
} device_connections_t;

typedef struct {
    uint64_t device_id;
    uint8_t
        types[MAX_BUTTONS];  // Button type per button (0=toggle, 1=stateful)
} button_types_t;

// Peer health tracking
typedef struct {
    uint64_t device_id;
    mesh_addr_t mac_addr;  // MAC address for direct routing
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
extern bool g_mesh_connected;
extern bool g_mesh_started;
extern bool g_is_root;
extern int g_mesh_layer;
extern uint64_t g_parent_id;

// MQTT (root only)
extern esp_mqtt_client_handle_t g_mqtt_client;
extern bool g_mqtt_connected;

// Routing configuration (root only)
extern device_connections_t g_connections[MAX_NODES];
extern uint8_t g_num_devices;
extern button_types_t g_button_types[MAX_NODES];  // Button type per device
                                                  // (0=toggle, 1=stateful)
extern SemaphoreHandle_t g_connections_mutex;
extern SemaphoreHandle_t g_button_types_mutex;

// Button state (switch nodes)
extern button_state_t g_button_states[NUM_BUTTONS];
extern const int g_button_pins[NUM_BUTTONS];
extern uint32_t g_last_root_contact;

// Relay state (relay nodes)
extern board_type_t g_board_type;
extern uint16_t g_relay_outputs;  // 16-bit state for all relays
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
extern bool g_ota_in_progress;
extern bool g_ota_requested;

// Broadcast address for mesh messages
extern mesh_addr_t g_broadcast_addr;

// ============ mesh_init.c ============
void mesh_network_init(void);
// Returns true when the station network interface is up (we have IP
// connectivity)
bool domator_mesh_is_wifi_connected(void);
// Stop mesh and switch to STA mode, try to connect to router for up to
// `timeout_ms` milliseconds. Returns true if connected, false on timeout.
bool mesh_stop_and_connect_sta(uint32_t timeout_ms);

// node_switch.c (switch node functions)
void button_init(void);
void button_task(void* arg);
void led_init(void);
void led_task(void* arg);
void led_set_color(uint8_t r, uint8_t g, uint8_t b);
void led_flash_cyan(void);
=======
    // ============ APP MESSAGE PROTOCOL ============
    typedef struct __attribute__((packed)){uint32_t src_id; uint8_t msg_type;
                                           uint8_t data_len; char data[200];}
mesh_app_msg_t;

// ============ GLOBALS ============
extern node_type_t g_node_type;
extern uint32_t g_device_id;
extern bool g_is_root;
extern bool g_mesh_connected;

// ============ mesh_init.c ============
void mesh_network_init(void);
>>>>>>> 75e1902 (changed to my version)

// ============ mesh_comm.c ============
void mesh_rx_task(void* arg);
void mesh_tx_task(void* arg);
<<<<<<< HEAD
void mesh_queue_to_node(mesh_app_msg_t* msg, tx_priority_t prio,
                        mesh_addr_t* dest);
=======
void mesh_queue_to_root(mesh_app_msg_t* msg);
void mesh_queue_to_node(mesh_addr_t* dest, mesh_app_msg_t* msg);
>>>>>>> 75e1902 (changed to my version)
void status_report_task(void* arg);

// ============ node_root.c ============
void root_handle_mesh_message(mesh_addr_t* from, mesh_app_msg_t* msg);
void node_root_start(void);
<<<<<<< HEAD
void mqtt_init(void);
void node_root_stop(void);
void root_publish_status(void);

// ============ node_relay.c ============
void relay_save_states_to_nvs(void);
void relay_board_detect(void);
void relay_init(void);
void relay_set(int index, bool state);
void relay_toggle(int index);
void relay_write_shift_register(uint16_t bits);
bool relay_get_state(int index);
void relay_sync_all_states(void);
void relay_send_state_confirmation(int index);
void relay_button_init(void);
void relay_button_task(void* arg);
void relay_handle_command(const char* cmd_data);

// ============ health_ota.c ============
void ota_task(void* arg);
void health_monitor_task(void* arg);

// ============ telnet.c ============
void telnet_task(void* arg);
int dual_log_vprintf(const char* fmt, va_list args);
void telnet_start(void);
void telnet_stop(void);
=======
void node_root_stop(void);
void node_root_mqtt_connect(void);
void root_publish_status(const char* payload);
>>>>>>> 75e1902 (changed to my version)
