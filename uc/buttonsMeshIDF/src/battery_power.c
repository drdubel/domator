#include "battery_power.h"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "BATTERY";

bool battery_usb_keeps_awake(int64_t* awake_until) {
    int64_t now = esp_timer_get_time();
    // SOF traffic identifies an actual computer, even when no terminal is
    // open. A USB charger or power bank alone does not keep the device awake.
    if (usb_serial_jtag_is_connected()) *awake_until = now + BATTERY_USB_GRACE_US;
    return now < *awake_until;
}

void battery_wait_for_provisioning(void) {
    const int64_t grace_us = 30LL * 1000000;
    int64_t sleep_after = esp_timer_get_time() + grace_us;
    ESP_LOGW(TAG, "Waiting for USB provisioning; reset after writing credentials. "
             "Sleeping after 30 seconds without a USB host.");
    while (true) {
        int64_t now = esp_timer_get_time();
        if (usb_serial_jtag_is_connected()) sleep_after = now + grace_us;
        else if (now >= sleep_after) {
            ESP_LOGW(TAG, "No USB host; sleeping until reset");
            esp_deep_sleep_start();
        }
        // FreeRTOS delay keeps USB running. Manual light sleep would also
        // interrupt the native USB connection and prevent provisioning.
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
