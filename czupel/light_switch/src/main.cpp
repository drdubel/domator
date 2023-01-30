#include <Arduino.h>
#include <ArduinoJson.h>

#define NLIGHTS 12

HardwareSerial serialDebug(PA10, PA9);
HardwareSerial serialEsp(PA3, PA2);

int switch1[NLIGHTS], whichDigit, whichLamp, i;
int whichLight;

void get_cmd(char cmd_char) {
    if (('A' <= cmd_char) && (cmd_char <= 'L')) {
        serialEsp.write('a' + (cmd_char - 'A'));
        serialEsp.write('0' + switch1[cmd_char - 'A']);
        serialDebug.write('a' + (cmd_char - 'A'));
        serialDebug.write('0' + switch1[cmd_char - 'A']);
    } else if (('a' <= cmd_char) && (cmd_char <= 'l')) {
        whichLight = cmd_char - 'a';
    } else {
        switch1[whichLight] = (int)cmd_char - '0';
    }
}

void setup() {
    serialDebug.begin(115200);
    serialEsp.begin(115200);
}

void loop() {
    if (serialDebug.available() > 0) {
        char cmd_char = serialDebug.read();
        serialDebug.print(cmd_char);
        get_cmd(cmd_char);
    }
    if (serialEsp.available() > 0) {
        char cmd_char = serialEsp.read();
        get_cmd(cmd_char);
    }
}