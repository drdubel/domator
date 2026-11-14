#include <assert.h>
#include <string.h>
#include "presence_schedule.h"

static presence_plan_t nightly(void) {
    // Paused 23:00-06:00 for a week, received at noon.
    presence_plan_t plan = {.id = 42, .length_s = 7 * 86400};
    for (int day = 0; day < 7; day++) {
        plan.off[day][0] = 11 * 3600 + day * 86400;
        plan.off[day][1] = 18 * 3600 + day * 86400;
    }
    plan.count = 7;
    return plan;
}

int main(void) {
    presence_plan_t none = {0};
    assert(plan_valid(&none) && plan_active(&none, 0) && plan_active(&none, UINT32_MAX));
    assert(plan_left(&none, 0) == 0);

    presence_plan_t plan = nightly();
    assert(plan_valid(&plan));
    assert(plan_active(&plan, 0) && plan_active(&plan, 11 * 3600 - 1));
    assert(!plan_active(&plan, 11 * 3600) && !plan_active(&plan, 18 * 3600 - 1));
    assert(plan_active(&plan, 18 * 3600));
    assert(!plan_active(&plan, 6 * 86400 + 12 * 3600));
    // A plan that has run out leaves the sensor working.
    assert(plan_active(&plan, 8 * 86400 + 12 * 3600) && plan_left(&plan, 8 * 86400) == 0);
    assert(plan_left(&plan, 86400) == 6 * 86400);

    // Malformed plans: overlapping, unordered, empty or outside the plan.
    presence_plan_t bad = nightly();
    bad.off[1][0] = bad.off[0][1] - 1;
    assert(!plan_valid(&bad));
    bad = nightly();
    bad.off[2][1] = bad.off[2][0];
    assert(!plan_valid(&bad));
    bad = nightly();
    bad.off[6][1] = bad.length_s + 1;
    assert(!plan_valid(&bad));
    bad = nightly();
    bad.length_s = PLAN_MAX_S + 1;
    assert(!plan_valid(&bad));
    bad = nightly();
    bad.count = PLAN_PERIODS + 1;
    assert(!plan_valid(&bad));
    bad = nightly();
    bad.id = 0;
    assert(!plan_valid(&bad));
    // Adjacent periods and one reaching the end of the plan are fine.
    presence_plan_t edge = {.id = 1, .length_s = 100, .count = 2, .off = {{0, 50}, {50, 100}}};
    assert(plan_valid(&edge) && !plan_active(&edge, 0) && !plan_active(&edge, 99) && plan_active(&edge, 100));

    // Without a pause every change is sent, as before schedules.
    bool reported = false;
    assert(presence_route(true, true, &reported) && reported);
    assert(presence_route(false, true, &reported) && !reported);
    // Presence starting during a pause switches nothing, nor does its end.
    assert(!presence_route(true, false, &reported) && !reported);
    assert(!presence_route(false, false, &reported));
    // Presence that started before the pause still ends normally.
    assert(presence_route(true, true, &reported) && reported);
    assert(presence_route(false, false, &reported) && !reported);
    // Once the pause is over, the area becoming clear is reported even if the
    // presence began during it: lights switched on by hand go off.
    assert(!presence_route(true, false, &reported));
    assert(presence_route(false, true, &reported) && !reported);
    // The first state after a restart: sent only while active.
    reported = false;
    assert(!presence_route(true, false, &reported));
    assert(!presence_route(false, false, &reported));
    assert(presence_route(false, true, &reported));
    return 0;
}
