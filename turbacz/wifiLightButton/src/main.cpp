#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <consts.h>
#include <credentials.h>
#include <painlessMesh.h>
#include <webpage.h>

void receivedCallback(const uint32_t& from, const String& msg);

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

IPAddress myIP(0, 0, 0, 0);
AsyncWebServer server(80);
painlessMesh mesh;

uint32_t rootId;

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
    pixels.setPixelColor(0, pixels.Color(r, g, b));
    pixels.show();
}

// DO NAPRAWY
void updateLedStatus() {
    bool wifi_ok = (WiFi.status() == WL_CONNECTED);
    bool server_ok = serverStarted;

    if (wifi_ok && server_ok)
        setLedColor(255, 255, 255);
    else if (wifi_ok)
        setLedColor(255, 0, 0);
    else if (server_ok)
        setLedColor(0, 255, 0);
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

    static unsigned long lastSendTime = 0;
    if (millis() - lastSendTime >= 2000) {
        lastSendTime = millis();
        mesh.sendBroadcast(String(char('a' + 1)));
    }

    for (int i = 0; i < NLIGHTS; i++) {
        int currentState = digitalRead(buttonPins[i]);

        if (millis() - lastTimeClick[i] < debounceDelay) {
            continue;
        }

        if (currentState == HIGH && lastButtonState[i] == LOW) {
            lastTimeClick[i] = millis();

            msg = 'a' + i;
            Serial.print("Publishing message: ");
            Serial.println(DEVICE_ID + msg);

            mesh.sendSingle(rootId, String(msg));
        }

        lastButtonState[i] = currentState;
    }
}