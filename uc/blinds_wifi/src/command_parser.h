#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Return a complete four-byte UART command, or reject without writing it.
static inline bool parse_blind_command(const uint8_t* payload, size_t length,
                                      char* output, size_t capacity) {
    if (!payload || !output || capacity < 5 || length < 2 || length > 4 ||
        payload[0] < 'a' || payload[0] > 'h') return false;
    unsigned position = 0;
    for (size_t i = 1; i < length; ++i) {
        if (payload[i] < '0' || payload[i] > '9') return false;
        position = position * 10 + payload[i] - '0';
    }
    if (position > 100) return false;
    return snprintf(output, capacity, "%c%03u", payload[0], position) == 4;
}
