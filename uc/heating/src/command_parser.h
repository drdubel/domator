#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <string.h>

// Match the backend's command grammar without throwing on malformed input.
// Physical operating limits and the independent cutoff are installation policy.
static inline bool parse_heating_command(const uint8_t* payload, size_t length,
                                         char* command, double* value) {
    if (!payload || !command || !value || length < 2 || length > 27 ||
        !strchr("pidtI", payload[0]) || payload[0] == 0) return false;
    size_t i = 1;
    if (payload[i] == '-') ++i;
    size_t integer_start = i;
    while (i < length && payload[i] >= '0' && payload[i] <= '9') ++i;
    if (i == integer_start || i - integer_start > 12) return false;
    if (i < length && payload[i] == '.') {
        size_t fraction_start = ++i;
        while (i < length && payload[i] >= '0' && payload[i] <= '9') ++i;
        if (i == fraction_start || i - fraction_start > 12) return false;
    }
    if (i != length) return false;
    char text[27];
    memcpy(text, payload + 1, length - 1);
    text[length - 1] = 0;
    errno = 0;
    char* end;
    double parsed = strtod(text, &end);
    if (*end || errno == ERANGE || !isfinite(parsed)) return false;
    *command = payload[0];
    *value = parsed;
    return true;
}
