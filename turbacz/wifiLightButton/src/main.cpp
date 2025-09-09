#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <credentials.h>
#include <painlessMesh.h>
#include <webpage.h>

#define HOSTNAME "20"
#define DEVICE_ID "20"

#define NLIGHTS 7
#define LED_PIN 8
#define NUM_LEDS 1

void receivedCallback(const uint32_t& from, const String& msg);

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

IPAddress myIP(0, 0, 0, 0);
AsyncWebServer server(80);
painlessMesh mesh;

uint32_t rootId;

const int buttonPins[NLIGHTS] = {A0, A1, A3, A4, A5, 6, 7};
int lastTimeClick[NLIGHTS];
int debounceDelay = 250;
char whichLight;
int lastButtonState[NLIGHTS] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};

char msg;

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
    pixels.setPixelColor(0, pixels.Color(r, g, b));
    pixels.show();
}

// DO NAPRAWY
void updateLedStatus() {
    bool wifi_ok = (WiFi.status() == WL_CONNECTED);

    if (wifi_ok)
        setLedColor(255, 255, 255);
    else
        setLedColor(0, 0, 0);
}

void meshInit() {
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);

    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA, 6);
    mesh.onReceive(&receivedCallback);
}

void receivedCallback(const uint32_t& from, const String& msg) {
    Serial.printf("bridge: Received from %u msg=%s\n", from, msg.c_str());

    rootId = msg.toInt();
    Serial.print("New root ID: ");
    Serial.println(rootId);

    mesh.sendSingle(rootId, String(DEVICE_ID));
}

void setup() {
    Serial.begin(115200);

    pixels.begin();
    pixels.setBrightness(5);
    setLedColor(0, 0, 0);

    meshInit();

    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(buttonPins[i], INPUT_PULLDOWN);
    }
}

void loop() {
    mesh.update();

    for (int i = 0; i < NLIGHTS; i++) {
        int currentState = digitalRead(buttonPins[i]);

        if (millis() - lastTimeClick[i] < debounceDelay) {
            continue;
        }

        if (currentState == HIGH && lastButtonState[i] == LOW) {
            lastTimeClick[i] = millis();

            msg = 'a' + i;
            Serial.print("Publishing message: ");
            Serial.println(msg);

            mesh.sendSingle(rootId, String(msg));
        }

        lastButtonState[i] = currentState;
    }
}