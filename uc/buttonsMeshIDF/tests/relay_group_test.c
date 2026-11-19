#include <assert.h>
#include "relay_group.h"

int main(void) {
    relay_states_t states = {0};
    route_target_t lamps[] = {{111, "a"}, {111, "b"}, {222, "p"}};

    // Nothing reported yet counts as off: the group turns on.
    assert(relay_states_get(&states, 111, 'a') == -1);
    assert(!relay_states_known(&states, 111));
    assert(relay_group_level(&states, lamps, 3) == '1');

    // Relays report upper case outputs, routes use lower case.
    relay_states_set(&states, 222, 'P', true);
    assert(relay_states_get(&states, 222, 'p') == 1 && relay_states_known(&states, 222));
    // Any of them on turns them all off, the others reported or not.
    assert(relay_group_level(&states, lamps, 3) == '0');
    relay_states_set(&states, 111, 'A', false);
    assert(relay_group_level(&states, lamps, 3) == '0');
    relay_states_set(&states, 222, 'P', false);
    assert(relay_group_level(&states, lamps, 3) == '1');
    relay_states_set(&states, 111, 'b', true);
    assert(relay_group_level(&states, lamps, 3) == '0');
    relay_states_set(&states, 111, 'a', true);
    relay_states_set(&states, 222, 'p', true);
    assert(relay_group_level(&states, lamps, 3) == '0');
    // A light outside the group does not count.
    for (int i = 0; i < 3; i++)
        relay_states_set(&states, lamps[i].target_node_id, lamps[i].relay_command[0], false);
    relay_states_set(&states, 111, 'c', true);
    relay_states_set(&states, 333, 'a', true);
    assert(relay_group_level(&states, lamps, 3) == '1');

    // Outputs and relays are independent.
    assert(relay_states_get(&states, 111, 'a') == 0 && relay_states_get(&states, 111, 'c') == 1);
    assert(relay_states_get(&states, 111, 'd') == -1 && relay_states_get(&states, 222, 'a') == -1);
    assert(relay_states_get(&states, 333, 'A') == 1);

    // Invalid outputs and IDs are not recorded, and do not create a relay.
    relay_states_set(&states, 444, 'q', true);
    relay_states_set(&states, 444, '1', true);
    relay_states_set(&states, 0, 'a', true);
    assert(!relay_states_known(&states, 444) && !relay_states_known(&states, 0));
    assert(relay_states_get(&states, 111, 'q') == -1 && relay_states_get(&states, 0, 'a') == -1);
    route_target_t seventeenth[] = {{111, "q"}};
    assert(relay_group_level(&states, seventeenth, 1) == '1');

    // A full table keeps updating its relays; further ones stay unreported.
    states = (relay_states_t){0};
    for (uint64_t id = 1; id <= RELAY_STATES_RELAYS + 1; id++) relay_states_set(&states, id, 'a', true);
    assert(relay_states_get(&states, RELAY_STATES_RELAYS, 'a') == 1);
    assert(relay_states_get(&states, RELAY_STATES_RELAYS + 1, 'a') == -1);
    assert(!relay_states_known(&states, RELAY_STATES_RELAYS + 1));
    relay_states_set(&states, 1, 'P', true);
    relay_states_set(&states, 1, 'a', false);
    assert(relay_states_get(&states, 1, 'a') == 0 && relay_states_get(&states, 1, 'p') == 1);
    return 0;
}
