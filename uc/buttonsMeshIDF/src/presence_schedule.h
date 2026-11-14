#pragma once
/* Daily pause of a presence sensor, set in RCM as times or sun events.
 *
 * The node has no wall clock, so Turbacz turns the schedule into a plan: the
 * paused periods of the next days, in seconds after the plan arrives. The
 * node follows it on its own timer while Turbacz is unreachable. Without a
 * plan (none set, lost in a restart, or run out) the sensor works at all
 * hours, and Turbacz sends a new one when the status report shows it.
 *
 * Command: {"radar":"schedule","id":N,"for":S,"off":[[from,to],...]}
 * id 0 clears the schedule (for 0, no periods).
 *
 * No IDF dependency: checked on the host. */
#include <stdbool.h>
#include <stdint.h>

#define PLAN_PERIODS 16
#define PLAN_MAX_S (14u * 86400u)

typedef struct {
    uint32_t id;        /* reported back, so Turbacz sees which plan is followed */
    uint32_t length_s;  /* the plan covers this long after it arrives */
    uint8_t count;
    uint32_t off[PLAN_PERIODS][2]; /* paused [from, to), seconds after arrival */
} presence_plan_t;

/* Periods must be ordered, disjoint, not empty and within the plan. */
static inline bool plan_valid(const presence_plan_t* plan) {
    if (plan->count > PLAN_PERIODS || plan->length_s > PLAN_MAX_S) return false;
    if (!plan->id && (plan->length_s || plan->count)) return false;
    uint32_t end = 0;
    for (int i = 0; i < plan->count; i++) {
        if (plan->off[i][0] < end || plan->off[i][1] <= plan->off[i][0] || plan->off[i][1] > plan->length_s)
            return false;
        end = plan->off[i][1];
    }
    return true;
}

/* Whether presence switches outputs @p elapsed_s after the plan arrived. */
static inline bool plan_active(const presence_plan_t* plan, uint32_t elapsed_s) {
    for (int i = 0; i < plan->count; i++)
        if (elapsed_s >= plan->off[i][0] && elapsed_s < plan->off[i][1]) return false;
    return true;
}

/* Seconds the plan still covers. */
static inline uint32_t plan_left(const presence_plan_t* plan, uint32_t elapsed_s) {
    return elapsed_s < plan->length_s ? plan->length_s - elapsed_s : 0;
}

/* Whether a change to @p presence is sent to the outputs. Presence that
 * starts during a pause is not; the area becoming clear is, while active and
 * whenever it ends presence the outputs were told about. @p reported is what
 * they were last told, and is updated when this returns true. */
static inline bool presence_route(bool presence, bool active, bool* reported) {
    if (presence ? !active : !active && !*reported) return false;
    *reported = presence;
    return true;
}
