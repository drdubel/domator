#pragma once
/* Outputs connected to one momentary button switch together: when any of
 * them is on the button turns them all off, and when all are off it turns
 * them on. Toggling each output instead keeps a group apart for good once one
 * of its lights has been switched on its own.
 *
 * The root decides from the states relays report to it after every change
 * (MSG_TYPE_RELAY_STATE). An output without a report yet counts as off, so the
 * outputs still end up alike.
 *
 * No IDF dependency: checked on the host. */
#include <stdbool.h>
#include <stdint.h>

#define RELAY_STATES_RELAYS 64   /* MAX_NODES */
#define RELAY_STATES_OUTPUTS 16  /* MAX_RELAYS_16 */

/** @brief One relay target within a button routing entry. */
typedef struct {
    uint64_t target_node_id;
    char relay_command[2];
} route_target_t;

/* Bit N is output N: 'a' in commands, 'A' in state reports. */
typedef struct {
    uint64_t id;
    uint16_t known, on;
} relay_outputs_t;

/* Filled from the start; entries are only ever cleared all together. */
typedef struct {
    relay_outputs_t relays[RELAY_STATES_RELAYS];
} relay_states_t;

static inline int relay_output_index(char output) {
    if (output >= 'a' && output < 'a' + RELAY_STATES_OUTPUTS) return output - 'a';
    if (output >= 'A' && output < 'A' + RELAY_STATES_OUTPUTS) return output - 'A';
    return -1;
}

/* Record an output state. Ignored for an invalid output, and for a new relay
 * once the table is full. */
static inline void relay_states_set(relay_states_t* s, uint64_t id, char output, bool on) {
    int index = relay_output_index(output);
    if (index < 0 || !id) return;
    for (int i = 0; i < RELAY_STATES_RELAYS; i++) {
        relay_outputs_t* relay = &s->relays[i];
        if (relay->id && relay->id != id) continue;
        uint16_t bit = 1u << index;
        relay->id = id;
        relay->known |= bit;
        if (on) relay->on |= bit;
        else relay->on &= ~bit;
        return;
    }
}

/* 1 if the output is on, 0 if it is off, -1 without a report. */
static inline int relay_states_get(const relay_states_t* s, uint64_t id, char output) {
    int index = relay_output_index(output);
    for (int i = 0; index >= 0 && i < RELAY_STATES_RELAYS && s->relays[i].id; i++) {
        const relay_outputs_t* relay = &s->relays[i];
        if (relay->id != id) continue;
        uint16_t bit = 1u << index;
        if (!(relay->known & bit)) return -1;
        return (relay->on & bit) != 0;
    }
    return -1;
}

/* Whether the relay has reported any of its outputs. */
static inline bool relay_states_known(const relay_states_t* s, uint64_t id) {
    for (int i = 0; i < RELAY_STATES_RELAYS && s->relays[i].id; i++)
        if (s->relays[i].id == id) return true;
    return false;
}

/* The level, '0' or '1', a momentary button sets on all of its outputs. */
static inline char relay_group_level(const relay_states_t* s, const route_target_t* targets,
                                     int count) {
    for (int i = 0; i < count; i++)
        if (relay_states_get(s, targets[i].target_node_id, targets[i].relay_command[0]) == 1)
            return '0';
    return '1';
}
