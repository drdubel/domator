#ifndef DOMATOR_MESH_H
#define DOMATOR_MESH_H

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_mesh.h"
#include "mqtt_client.h"

// ====================
// Constants and Limits
// ====================

#define STATUS_REPORT_INTERVAL_MS       15000
#define BUTTON_POLL_INTERVAL_MS         20
#define BUTTON_DEBOUNCE_MS              250
#define LED_UPDATE_INTERVAL_MS          100
#define LED_FLASH_DURATION_MS           50

#define NUM_BUTTONS                     7
#define MAX_QUEUE_SIZE                  30
#define MESH_TX_QUEUE_SIZE              20
#define MESH_MSG_DATA_SIZE              200
#define LOW_HEAP_THRESHOLD              40000
#define CRITICAL_HEAP_THRESHOLD         20000

// GPIO pin definitions for ESP32-C3 switch board
#define BUTTON_GPIO_0                   0
#define BUTTON_GPIO_1                   1
#define BUTTON_GPIO_2                   3
#define BUTTON_GPIO_3                   4
#define BUTTON_GPIO_4                   5
#define BUTTON_GPIO_5                   6
#define BUTTON_GPIO_6                   7
#define LED_GPIO                        8

// Message types for mesh communication
#define MSG_TYPE_BUTTON                 'B'
#define MSG_TYPE_STATUS                 'S'
#define MSG_TYPE_COMMAND                'C'
#define MSG_TYPE_ACK                    'A'

// Node types
typedef enum {
    NODE_TYPE_UNKNOWN = 0,
    NODE_TYPE_ROOT,
    NODE_TYPE_SWITCH,
    NODE_TYPE_RELAY
} node_type_t;

// ====================
// Data Structures
// ====================

// Mesh application message structure
typedef struct {
    uint8_t msg_type;        // Message type (B/S/C/A)
    uint32_t device_id;      // Sender device ID
    uint16_t data_len;       // Length of data
    uint8_t data[MESH_MSG_DATA_SIZE];       // Payload data
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
} button_state_t;

// LED color
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

// ====================
// Global Variables (extern)
// ====================

// Device info
extern uint32_t g_device_id;
extern node_type_t g_node_type;
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

// Button state (switch nodes)
extern button_state_t g_button_states[NUM_BUTTONS];
extern const int g_button_pins[NUM_BUTTONS];

// Queues and mutexes
extern QueueHandle_t g_mesh_tx_queue;
extern SemaphoreHandle_t g_stats_mutex;

// OTA flag
extern bool g_ota_in_progress;

// ====================
// Function Declarations
// ====================

// domator_mesh.c
void app_main(void);
void generate_device_id(void);
void generate_firmware_hash(void);
void detect_hardware_type(void);

// mesh_init.c
void mesh_init(void);
void wifi_init(void);
esp_err_t mesh_event_handler(mesh_event_t event);

// mesh_comm.c
void mesh_send_task(void *arg);
void mesh_recv_task(void *arg);
void status_report_task(void *arg);
esp_err_t mesh_queue_to_root(const mesh_app_msg_t *msg);
void handle_mesh_recv(const mesh_addr_t *from, const mesh_app_msg_t *msg);

// node_root.c (root node functions)
void mqtt_init(void);
void mqtt_event_handler(void *handler_args, esp_event_base_t base, 
                        int32_t event_id, void *event_data);
void root_handle_mesh_message(const mesh_addr_t *from, const mesh_app_msg_t *msg);
void root_publish_status(void);
void root_forward_leaf_status(const char *json_str);

// node_switch.c (switch node functions)
void button_init(void);
void button_task(void *arg);
void led_init(void);
void led_task(void *arg);
void led_set_color(uint8_t r, uint8_t g, uint8_t b);
void led_flash_cyan(void);

#endif // DOMATOR_MESH_H
