#include <assert.h>
#include "remote_button.h"

static remote_button_event_t commit(remote_button_t* b, int value, int64_t at) {
    b->candidate = value;
    b->changed = at;
    return remote_button_commit(b, 500000, 150000, 4000000);
}

int main(void) {
    const remote_button_timing_t timing = {50000, 500000, 150000, 4000000};
    remote_button_t b = {.candidate = 1, .changed = 1000000};
    assert(!remote_button_ready(&b, 1049999, 50000));
    assert(remote_button_ready(&b, 1050000, 50000));
    remote_button_event_t event = commit(&b, 1, 1000000);
    assert(event.value == 1 && !event.long_press && !event.ota);

    // RTC state survives CPU reboots; release after a long deep sleep must
    // still be a long press, and must not independently trigger OTA.
    remote_button_t restored = b;
    event = commit(&restored, 0, 61000000);
    assert(!event.value && event.long_press && !event.ota);
    assert(!remote_button_ready(&restored, 62000000, 50000));

    // Short tap, then a second press within 150 ms and a hold across sleep.
    b = (remote_button_t){0};
    commit(&b, 1, 1000000);
    event = commit(&b, 0, 1100000);
    assert(!event.long_press && !event.ota);
    commit(&b, 1, 1200000);
    restored = b;
    event = commit(&restored, 0, 5300000);
    assert(event.long_press && event.ota);
    // A later press cannot inherit the previous OTA arm.
    commit(&restored, 1, 6000000);
    event = commit(&restored, 0, 11000000);
    assert(event.long_press && !event.ota);

    // A tap on ANY input can complete while another packet awaits its ACK.
    // Replaying the queued edges must produce both press and release, even
    // though the physical pin is already low when the sender becomes idle.
    for (int i = 0; i < 4; i++) {
        remote_button_t inputs[4] = {0};
        assert(!remote_button_observe(&inputs[i], 1, 1000000, &timing, &event));
        assert(remote_button_observe(&inputs[i], 0, 1080000, &timing, &event));
        assert(event.value == 1 && !event.long_press);
        assert(inputs[i].stable == 1 && inputs[i].candidate == 0);
        assert(remote_button_ready(&inputs[i], 1140000, timing.debounce_us));
        event = remote_button_commit(&inputs[i], timing.long_press_us,
            timing.ota_interval_us, timing.ota_hold_us);
        assert(event.value == 0 && !event.long_press && !event.ota);
        assert(!remote_button_ready(&inputs[i], 1140000, timing.debounce_us));
        for (int j = 0; j < 4; j++) {
            if (j != i) assert(inputs[j].stable == 0 && inputs[j].candidate == 0);
        }
    }

    // Overlapping b/c taps remain independent during a delayed radio ACK.
    remote_button_t overlap[4] = {0};
    assert(!remote_button_observe(&overlap[1], 1, 1000000, &timing, &event));
    assert(!remote_button_observe(&overlap[2], 1, 1010000, &timing, &event));
    assert(remote_button_observe(&overlap[2], 0, 1090000, &timing, &event));
    assert(event.value == 1 && overlap[1].stable == 0);
    assert(remote_button_observe(&overlap[1], 0, 1100000, &timing, &event));
    assert(event.value == 1);
    for (int i = 1; i <= 2; i++) {
        assert(remote_button_ready(&overlap[i], 1160000, timing.debounce_us));
        event = remote_button_commit(&overlap[i], timing.long_press_us,
            timing.ota_interval_us, timing.ota_hold_us);
        assert(event.value == 0 && !event.long_press && !event.ota);
    }

    // An old interrupt queued before a newer GPIO sample cannot resurrect a
    // released button, create a phantom press, or move its timestamp backward.
    b = (remote_button_t){.changed = 2000000};
    assert(!remote_button_observe(&b, 1, 1900000, &timing, &event));
    assert(b.changed == 2000000 && b.candidate == 0 && b.stable == 0);

    // Contact bounce shorter than debounce must not produce a transmission.
    b = (remote_button_t){0};
    assert(!remote_button_observe(&b, 1, 1000000, &timing, &event));
    assert(!remote_button_observe(&b, 0, 1020000, &timing, &event));
    assert(!remote_button_ready(&b, 1100000, timing.debounce_us));

    const int pins[] = {4, 5, 6, 7};
    remote_button_t buttons[4] = {0};
    assert(remote_button_wake_high_mask(pins, buttons, 4) == 0xf0);
    buttons[0].candidate = buttons[2].candidate = 1;
    // GPIO4/6 must wake LOW (release), GPIO5/7 HIGH (press).
    assert(remote_button_wake_high_mask(pins, buttons, 4) == 0xa0);
    for (int i = 0; i < 4; i++) buttons[i].candidate = 1;
    assert(remote_button_wake_high_mask(pins, buttons, 4) == 0);
    return 0;
}
