#pragma once

#include <stdbool.h>
#include <stdint.h>

/* C6-only helpers. Deadlines use esp_timer time within the current boot. */
#define BATTERY_USB_GRACE_US (3LL * 1000000)

/* Keep manual light/deep sleep disabled while a USB host is connected, with
 * a short grace period for enumeration and brief gaps in host traffic. */
bool battery_usb_keeps_awake(int64_t* awake_until);

/* Keep an invalid/unprovisioned board accessible over USB. After 30 seconds
 * without a USB host, sleep until reset to avoid draining a disconnected cell. */
void battery_wait_for_provisioning(void) __attribute__((noreturn));
