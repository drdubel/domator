#include <assert.h>
#include <string.h>
#include <initializer_list>
#include "command_parser.h"

int main() {
    char out[5];
    for (const char* input : {"a0", "a50", "h100", "a000"}) {
        assert(parse_blind_command((const uint8_t*)input, strlen(input), out, sizeof(out)));
        assert(strlen(out) == 4);
    }
    assert(strcmp(out, "a000") == 0);
    for (const char* input : {"", "a", "S", "z50", "a101", "a-1", "a+1", "a1x", "a9999999999999"})
        assert(!parse_blind_command((const uint8_t*)input, strlen(input), out, sizeof(out)));
    assert(!parse_blind_command(nullptr, 4, out, sizeof(out)));
    assert(!parse_blind_command((const uint8_t*)"a100", 4, out, 4));
    // Exhaust all short byte combinations under sanitizers.
    uint8_t payload[4] = {'a', 0, 0, 0};
    for (unsigned a = 0; a < 256; ++a)
        for (unsigned b = 0; b < 256; ++b) {
            payload[1] = a; payload[2] = b;
            parse_blind_command(payload, 3, out, sizeof(out));
        }
}
