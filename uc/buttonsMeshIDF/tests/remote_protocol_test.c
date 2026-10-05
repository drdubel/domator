#include <assert.h>
#include <string.h>
#include "remote_protocol.h"

int main(void) {
    remote_packet_t p = {.magic = REMOTE_MAGIC, .version = REMOTE_VERSION,
        .kind = REMOTE_UPLINK, .device_id = 123, .session = 42, .sequence = 1,
        .length = 3, .payload = {'B', 'a', '1'}};
    assert(sizeof(p) == 250);
    assert(remote_uplink_valid(&p));
    p.payload[1] = 'h'; assert(!remote_uplink_valid(&p)); p.payload[1] = 'a';
    p.payload[2] = '2'; assert(!remote_uplink_valid(&p)); p.payload[2] = '0';
    p.length = 4; p.payload[3] = '1'; assert(remote_uplink_valid(&p));
    p.payload[3] = 'x'; assert(!remote_uplink_valid(&p));
    p.payload[0] = 'P'; p.length = 3; assert(remote_uplink_valid(&p));
    p.length = 2; assert(!remote_uplink_valid(&p));
    p.payload[0] = 'U'; assert(!remote_uplink_valid(&p));
    p.length = REMOTE_PAYLOAD_SIZE + 1; assert(!remote_packet_shape_valid(&p));
    p.length = 3; p.version++; assert(!remote_packet_shape_valid(&p)); p.version--;
    p.device_id = 0; assert(!remote_packet_shape_valid(&p)); p.device_id = 123;
    p.kind = REMOTE_ACK; assert(!remote_uplink_valid(&p)); p.kind = REMOTE_UPLINK;
    remote_history_t h = {0};
    assert(!remote_duplicate(&h, &p));
    assert(remote_duplicate(&h, &p)); // Lost ACK: retry cannot toggle twice.
    p.sequence = 2; assert(!remote_duplicate(&h, &p));
    p.sequence = 1; assert(remote_duplicate(&h, &p)); // Late copy of an old event.
    p.session++; assert(!remote_duplicate(&h, &p)); // Reboot resets sequence safely.
    assert(remote_duplicate(&h, &p));
    return 0;
}
