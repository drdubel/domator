#include <Arduino.h>

#define NLIGHTS 12

HardwareSerial serialDebug(PA10, PA9);
HardwareSerial serialEsp(PA3, PA2);

const int buttonPin = PA0;
const int ledPin = PC13;
const int checkDelay = 200;

bool switch1[NLIGHTS];
int whichDigit, whichLight, buttonState, lastCheck, prevButtonState, i;

void get_cmd(char cmd_char) {
    if (('A' <= cmd_char) && (cmd_char <= 'L')) {
        serialEsp.write('a' + (cmd_char - 'A'));
        serialEsp.write('0' + (int)switch1[cmd_char - 'A']);
        serialDebug.write('a' + (cmd_char - 'A'));
        serialDebug.write('0' + (int)switch1[cmd_char - 'A']);
    } else if (('a' <= cmd_char) && (cmd_char <= 'l')) {
        whichLight = cmd_char - 'a';
    } else {
        switch1[whichLight] = (bool)(cmd_char - '0');
        if (whichLight == 0) digitalWrite(ledPin, !switch1[whichLight]);
    }
}

void setup() {
    serialDebug.begin(115200);
    serialEsp.begin(115200);
    pinMode(ledPin, OUTPUT);
    pinMode(buttonPin, INPUT_PULLUP);
    digitalWrite(ledPin, HIGH);
}

void loop() {
    buttonState = digitalRead(buttonPin);
    if ((buttonState == LOW) && (millis() - lastCheck > checkDelay) &&
        (buttonState != prevButtonState)) {
        switch1[0] ? switch1[0] = 0 : switch1[0] = 1;
        digitalWrite(ledPin, !switch1[0]);
        serialEsp.write("a");
        serialEsp.write('0' + (int)switch1[0]);
        serialDebug.write("a");
        serialDebug.write('0' + (int)switch1[0]);
        prevButtonState = LOW;
        lastCheck = millis();
    }
    prevButtonState = digitalRead(buttonPin);
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