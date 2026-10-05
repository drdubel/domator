#include "mesh_protocol.h"
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    mesh_app_msg_t msg;
    if (size != sizeof(msg)) return 0;
    memcpy(&msg, data, size);
    if (mesh_message_valid(&msg, size)) mesh_terminate_text(&msg);
    return 0;
}
