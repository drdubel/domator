#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Plain data: retained in RTC RAM on C6, ordinary RAM on C3. Times use the
 * same continuous clock across sleep, including while a switch stays held. */
typedef struct {
    int stable, candidate;
    int64_t changed, pressed, released;
    bool ota_armed;
} remote_button_t;

typedef struct { int value; bool long_press, ota; } remote_button_event_t;

typedef struct {
    int64_t debounce_us, long_press_us, ota_interval_us, ota_hold_us;
} remote_button_timing_t;

static inline bool remote_button_ready(const remote_button_t* b, int64_t now,
                                       int64_t debounce_us) {
    return b->candidate != b->stable && now - b->changed >= debounce_us;
}

static inline remote_button_event_t remote_button_commit(remote_button_t* b,
        int64_t long_press_us, int64_t ota_interval_us, int64_t ota_hold_us) {
    b->stable = b->candidate;
    bool ota = false;
    if (b->stable) {
        b->ota_armed = b->released && b->changed - b->released < ota_interval_us;
        b->pressed = b->changed;
    } else {
        b->released = b->changed;
        ota = b->ota_armed && b->changed - b->pressed > ota_hold_us;
    }
    return (remote_button_event_t){b->stable,
        !b->stable && b->changed - b->pressed >= long_press_us, ota};
}

/* Apply timestamped input without doing any radio work. A complete press
 * queued while a packet was in flight must be emitted before its release
 * becomes the next candidate. Ignore an ISR edge older than our last sample. */
static inline bool remote_button_observe(remote_button_t* b, int value, int64_t at,
        const remote_button_timing_t* timing, remote_button_event_t* event) {
    if (at < b->changed || value == b->candidate) return false;
    bool ready = remote_button_ready(b, at, timing->debounce_us);
    if (ready) *event = remote_button_commit(b, timing->long_press_us,
        timing->ota_interval_us, timing->ota_hold_us);
    b->candidate = value;
    b->changed = at;
    return ready;
}

/* Wake on the opposite level of EVERY input, including held switches. */
static inline uint64_t remote_button_wake_high_mask(const int* pins,
        const remote_button_t* buttons, int count) {
    uint64_t mask = 0;
    for (int i = 0; i < count; i++)
        if (!buttons[i].candidate) mask |= UINT64_C(1) << pins[i];
    return mask;
}
