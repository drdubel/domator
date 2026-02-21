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

void telnet_start(void) {
    if (telnet_task_handle != NULL) return;

    // Initialize log mutex before enabling dual logger
    if (log_mutex == NULL) {
        log_mutex = xSemaphoreCreateMutex();
    }

    // Enable dual logging (serial + telnet) now that telnet will be started
    // Save previous vprintf so we can restore it when stopping telnet
    prev_log_vprintf = esp_log_set_vprintf(dual_log_vprintf);

    xTaskCreate(telnet_task, "telnet", 8192, NULL, 5, &telnet_task_handle);
}

void telnet_stop(void) {
    close(telnet_sock);
    telnet_sock = -1;

    // Restore previous log vprintf if we replaced it
    if (prev_log_vprintf != NULL) {
        esp_log_set_vprintf(prev_log_vprintf);
        prev_log_vprintf = NULL;
    }

    // Disable dual logging (restore default logger)
    esp_log_set_vprintf(NULL);

    if (telnet_task_handle != NULL) {
        vTaskDelete(telnet_task_handle);
        telnet_task_handle = NULL;
    }
}

void telnet_task(void* arg) {
    int listen_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char rxbuf[RX_BUF_SIZE];

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TELNET_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(listen_sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    listen(listen_sock, 1);

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

int dual_log_vprintf(const char* fmt, va_list args) {
    int len = 0;
    va_list args_copy;

    // First try with a small stack buffer to avoid heap allocations most of the
    // time
    char small_buf[256];
    va_copy(args_copy, args);
    len = vsnprintf(small_buf, sizeof(small_buf), fmt, args_copy);
    va_end(args_copy);

    if (len < 0) return len;

    char* out_buf = NULL;
    bool used_heap = false;

    if (len >= (int)sizeof(small_buf)) {
        // Need larger buffer, allocate on heap
        out_buf = malloc(len + 1);
        if (out_buf == NULL) {
            // Allocation failed, truncate to small buffer
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

    xSemaphoreTake(log_mutex, portMAX_DELAY);

    // Always output to serial
    if (used_heap) {
        fwrite(out_buf, 1, len, stdout);
    } else {
        fwrite(small_buf, 1, len, stdout);
    }

    // Also output to telnet if connected
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