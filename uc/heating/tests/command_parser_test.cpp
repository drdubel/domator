#include <assert.h>
#include <initializer_list>
#include "command_parser.h"
int main() {
    char command;
    double value;
    for (const char* input : {"t40", "p0.05", "I-3", "d0"})
        assert(parse_heating_command((const uint8_t*)input, strlen(input), &command, &value));
    for (const char* input : {"", "t", "t-", "tNaN", "tinf", "t1e400", "p 5", "x5", "t1bad", "t1.", "t1234567890123"})
        assert(!parse_heating_command((const uint8_t*)input, strlen(input), &command, &value));
    uint8_t payload[3] = {'t', 0, 0};
    for (unsigned a = 0; a < 256; a++)
        for (unsigned b = 0; b < 256; b++) {
            payload[1] = a; payload[2] = b;
            parse_heating_command(payload, sizeof(payload), &command, &value);
        }
}
