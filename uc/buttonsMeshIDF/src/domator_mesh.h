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

// ============ NODE TYPES ============
typedef enum {
    NODE_TYPE_UNKNOWN = 0,
    NODE_TYPE_SWITCH_C3,
    NODE_TYPE_RELAY_8,
    NODE_TYPE_RELAY_16,
} node_type_t;

// ============ APP MESSAGE PROTOCOL ============
typedef struct __attribute__((packed)) {
    uint32_t src_id;
    uint8_t msg_type;
    uint8_t data_len;
    char data[200];
} mesh_app_msg_t;

// ============ GLOBALS ============
extern node_type_t g_node_type;
extern uint32_t g_device_id;
extern bool g_is_root;
extern bool g_mesh_connected;

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
void node_root_stop(void);
void node_root_mqtt_connect(void);
void root_publish_status(const char* payload);