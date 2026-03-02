/**
 * @file telnet.c
 * @brief Minimal Telnet server and dual UART/Telnet log mirror.
 *
 * When the root node gains an IP address, telnet_start() is called to:
 *  - Install dual_log_vprintf() as the ESP-IDF log backend so every log line
 *    is written to both UART and the active Telnet socket.
 *  - Launch telnet_task() which listens on TCP port 23, accepts one client
 *    at a time, and echoes received data back to the client.
 *
 * telnet_stop() (called when the node loses root status) tears down the
 * server socket, deletes the task, and restores the default log handler.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "domator_mesh.h"
#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "linenoise/linenoise.h"
#include "lwip/sockets.h"

#define TELNET_PORT 23
#define RX_BUF_SIZE 256

static const char* TAG = "TELNET";

int telnet_sock = -1;

static SemaphoreHandle_t log_mutex;
static int (*prev_log_vprintf)(const char* fmt, va_list args) = NULL;

// ====================
// Telnet Lifecycle
// ====================

/**
 * @brief Start the Telnet server task and enable dual UART/Telnet logging.
 *        Idempotent: does nothing if telnet_task_handle is already set.
 *        Creates the log mutex on first invocation.
 */
void telnet_start(void) {
    if (telnet_task_handle != NULL) return;

    if (log_mutex == NULL) {
        log_mutex = xSemaphoreCreateMutex();
    }

    prev_log_vprintf = esp_log_set_vprintf(dual_log_vprintf);

    xTaskCreate(telnet_task, "telnet", 8192, NULL, 5, &telnet_task_handle);
}

/**
 * @brief Stop the Telnet server, close the active socket, delete the task,
 *        and restore the previous log vprintf handler.
 */
void telnet_stop(void) {
    if (telnet_task_handle != NULL) {
        close(telnet_sock);
        telnet_sock = -1;

        if (prev_log_vprintf != NULL) {
            esp_log_set_vprintf(prev_log_vprintf);
            prev_log_vprintf = NULL;
        }

        vTaskDelete(telnet_task_handle);
        telnet_task_handle = NULL;
    }
}

// ====================
// Telnet Server Task
// ====================

/**
 * @brief FreeRTOS task: run the Telnet TCP server loop.
 *
 * Binds to INADDR_ANY:TELNET_PORT, accepts one client at a time, and
 * echoes all received data back ("dumb terminal" mode).  Loops back to
 * accept() after each client disconnects.
 */
void telnet_task(void* arg) {
    int listen_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char rxbuf[RX_BUF_SIZE];

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TELNET_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "Socket listen failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Telnet server listening on port %d", TELNET_PORT);

    while (1) {
        telnet_sock =
            accept(listen_sock, (struct sockaddr*)&client_addr, &addr_len);

        ESP_LOGI(TAG, "Client connected");

        while (1) {
            int len = recv(telnet_sock, rxbuf, sizeof(rxbuf) - 1, 0);

            if (len <= 0) break;

            rxbuf[len] = 0;

            printf("%s", rxbuf);               // echo to console/log
            send(telnet_sock, rxbuf, len, 0);  // echo back
        }

        close(telnet_sock);
        telnet_sock = -1;

        ESP_LOGI(TAG, "Client disconnected");
    }
}

// ====================
// Dual Log Handler
// ====================

/**
 * @brief Custom ESP-IDF vprintf log handler that writes to UART and, if a
 *        Telnet client is connected, also to the Telnet socket.
 *
 * Uses a small stack buffer for the common case; falls back to a heap
 * allocation for messages that exceed 256 bytes.  Serialised by log_mutex
 * to prevent interleaved output from multiple tasks.
 *
 * @param fmt printf-style format string.
 * @param args Variadic argument list.
 * @return Number of characters written.
 */
int dual_log_vprintf(const char* fmt, va_list args) {
    int len = 0;
    va_list args_copy;

    char small_buf[256];
    va_copy(args_copy, args);
    len = vsnprintf(small_buf, sizeof(small_buf), fmt, args_copy);
    va_end(args_copy);

    if (len < 0) return len;

    char* out_buf = NULL;
    bool used_heap = false;

    if (len >= (int)sizeof(small_buf)) {
        out_buf = malloc(len + 1);
        if (out_buf == NULL) {
            len = sizeof(small_buf) - 1;
            small_buf[len] = '\0';
        } else {
            va_copy(args_copy, args);
            vsnprintf(out_buf, len + 1, fmt, args_copy);
            va_end(args_copy);
            used_heap = true;
        }
    }

    if (log_mutex == NULL) {
        log_mutex = xSemaphoreCreateMutex();
    }

    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        // Fallback: write to UART only
        if (used_heap) {
            fwrite(out_buf, 1, len, stdout);
        } else {
            fwrite(small_buf, 1, len, stdout);
        }
        if (used_heap) free(out_buf);
        return len;
    }

    if (used_heap) {
        fwrite(out_buf, 1, len, stdout);
    } else {
        fwrite(small_buf, 1, len, stdout);
    }

    if (telnet_sock >= 0) {
        if (used_heap) {
            send(telnet_sock, out_buf, len, MSG_DONTWAIT);
        } else {
            send(telnet_sock, small_buf, len, MSG_DONTWAIT);
        }
    }

    xSemaphoreGive(log_mutex);

    if (used_heap) free(out_buf);

    return len;
}