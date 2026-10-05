#include <assert.h>
#include "mesh_protocol.h"

int main(void) {
    mesh_app_msg_t msg = {.src_id = 1, .msg_type = MSG_TYPE_BUTTON, .data_len = 2, .data = {'a', '1'}};
    assert(mesh_message_valid(&msg, sizeof(msg)));
    assert(!mesh_message_valid(&msg, sizeof(msg)-1));
    assert(!mesh_message_valid(&msg, sizeof(msg)+1));
    msg.msg_type = MSG_TYPE_COMMAND; msg.data_len = 511;
    memset(msg.data, 'a', sizeof(msg.data));
    assert(mesh_message_valid(&msg, sizeof(msg)));
    mesh_terminate_text(&msg); assert(msg.data[511] == 0);
    msg.data_len = 512; assert(!mesh_message_valid(&msg, sizeof(msg)));
    for (unsigned type = 0; type < 256; type++)
        for (unsigned length = 0; length < 65536; length++) {
            msg.msg_type = type; msg.data_len = length;
            if (mesh_message_valid(&msg, sizeof(msg))) mesh_terminate_text(&msg);
        }
    msg.msg_type = MSG_TYPE_TYPE_INFO; msg.data_len = 1; msg.data[0] = DEVICE_TYPE_RELAY;
    assert(mesh_message_valid(&msg, sizeof(msg)));
    msg.target_type = '?'; assert(!mesh_message_valid(&msg, sizeof(msg)));
}
