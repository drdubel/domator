#include <Arduino.h>

#define NLIGHTS 9

HardwareSerial serialDebug(PA10, PA9);
HardwareSerial serialEsp(PA3, PA2);

const int lightPins[NLIGHTS] = {PB9, PB8, PB7, PB6, PB5, PB4, PB3, PA15, PC13};

bool switch1[NLIGHTS];
int whichDigit, whichLight, buttonState, lastCheck, prevButtonState, i;

void get_cmd(char cmd_char) {
    if (('A' <= cmd_char) && (cmd_char <= 'A' + NLIGHTS)) {
        serialEsp.write('a' + (cmd_char - 'A'));
        serialEsp.write('0' + (int)switch1[cmd_char - 'A']);
        serialDebug.write('a' + (cmd_char - 'A'));
        serialDebug.write('0' + (int)switch1[cmd_char - 'A']);
    } else if (('a' <= cmd_char) && (cmd_char <= 'l')) {
        whichLight = cmd_char - 'a';
    } else {
        switch1[whichLight] = (bool)(cmd_char - '0');
        digitalWrite(lightPins[whichLight], switch1[whichLight]);
        serialEsp.write('a' + whichLight);
        serialEsp.write('0' + (int)switch1[whichLight]);
        serialDebug.write('a' + whichLight);
        serialDebug.write('0' + (int)switch1[whichLight]);
    }
}

void setup() {
    serialDebug.begin(115200);
    serialEsp.begin(115200);
    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(lightPins[i], OUTPUT);
        digitalWrite(lightPins[i], LOW);
    }
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
