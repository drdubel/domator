#include <inttypes.h>
#include <stdio.h>

#include "credentials.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#define NBUTTONS 7

int button_pins[NBUTTONS] = {0, 1, 2, 3, 4, 5, 6};

static const char* TAG = "MQTT_BUTTONS_ISR";
char device_id[32];
char topic[64];

esp_mqtt_client_handle_t client;

static QueueHandle_t gpio_evt_queue;

static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                               int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to MQTT broker");
            esp_mqtt_client_publish(client, topic, "Hello from ESP32-C3", 0, 1,
                                    0);
            esp_mqtt_client_subscribe(client, "/switch/cmd", 0);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Received data: %.*s", event->data_len, event->data);
            break;

        default:
            break;
    }
}

static void wifi_init(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .sta =
            {
                .ssid = SSID,
                .password = PASSWORD,
            },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();
}

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    int gpio_num = (int)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void generate_device_id(void) {
    uint8_t mac[6];
    esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);

    uint64_t mac_decimal = 0;
    for (int i = 0; i < 6; i++) {
        mac_decimal = (mac_decimal << 8) | mac[i];
    }

    snprintf(device_id, sizeof(device_id), "%" PRIu64, mac_decimal);
    snprintf(topic, sizeof(topic), "/switch/%s", device_id);

    printf("Device ID (decimal): %s\n", device_id);
    printf("MQTT Topic: %s\n", topic);
}

static void gpio_task(void* arg) {
    int gpio_num;
    while (1) {
        if (xQueueReceive(gpio_evt_queue, &gpio_num, portMAX_DELAY)) {
            vTaskDelay(pdMS_TO_TICKS(140));
            if (gpio_get_level(gpio_num) == 1) {
                for (int i = 0; i < NBUTTONS; i++) {
                    if (button_pins[i] == gpio_num) {
                        char msg = 'a' + i;
                        ESP_LOGI(TAG,
                                 "Button %d pressed -> sending '%c' topic: %s",
                                 i, msg, topic);
                        esp_mqtt_client_publish(client, topic, &msg, 0, 1, 0);
                        break;
                    }
                }
            }
        }
    }
}

static void buttons_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };

    for (int i = 0; i < NBUTTONS; i++) {
        io_conf.pin_bit_mask = 1ULL << button_pins[i];
        gpio_config(&io_conf);
    }

    gpio_evt_queue = xQueueCreate(10, sizeof(int));

    xTaskCreate(gpio_task, "gpio_task", 4096, NULL, 10, NULL);

    gpio_install_isr_service(0);
    for (int i = 0; i < NBUTTONS; i++) {
        gpio_isr_handler_add(button_pins[i], gpio_isr_handler,
                             (void*)button_pins[i]);
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    wifi_init();
    vTaskDelay(pdMS_TO_TICKS(500));
    generate_device_id();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASSWORD,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler,
                                   NULL);
    esp_mqtt_client_start(client);

    buttons_init();
}