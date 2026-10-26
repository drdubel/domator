#include <assert.h>
#include "remote_button.h"

static remote_button_event_t commit(remote_button_t* b, int value, int64_t at) {
    b->candidate = value;
    b->changed = at;
    return remote_button_commit(b, 500000, 150000, 4000000);
}

int main(void) {
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
