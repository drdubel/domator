#include <stdarg.h>
#include <stdio.h>
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

void telnet_start(void) {
    if (telnet_task_handle != NULL) return;

    // Initialize log mutex before enabling dual logger
    if (log_mutex == NULL) {
        log_mutex = xSemaphoreCreateMutex();
    }

    // Enable dual logging (serial + telnet) now that telnet will be started
    esp_log_set_vprintf(dual_log_vprintf);

    xTaskCreate(telnet_task, "telnet", 8192, NULL, 5, &telnet_task_handle);
}

void telnet_stop(void) {
    close(telnet_sock);
    telnet_sock = -1;

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
    char buffer[2048];

    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);

    if (log_mutex == NULL) {
        log_mutex = xSemaphoreCreateMutex();
    }

    xSemaphoreTake(log_mutex, portMAX_DELAY);

    // Always output to serial
    fwrite(buffer, 1, len, stdout);

    // Also output to telnet if connected
    if (telnet_sock >= 0) {
        send(telnet_sock, buffer, len, MSG_DONTWAIT);
    }

    xSemaphoreGive(log_mutex);

    return len;
}