#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
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

const char* firmware_url = "https://czupel.dry.pl/static/data/firmware.bin";

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

void performFirmwareUpdate() {
    Serial.println("[OTA] Stopping mesh...");
    mesh.stop();  // stop mesh to free Wi-Fi

    Serial.println("[OTA] Switching to STA mode...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(STATION_SSID, STATION_PASSWORD);

    Serial.print("[OTA] Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
    }
    Serial.println(" connected!");

    WiFiClientSecure client;
    client
        .setInsecure();  // disable cert check (use proper cert in production!)
    HTTPClient http;

    Serial.println("[OTA] Connecting to update server...");
    if (http.begin(client, firmware_url)) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            int contentLength = http.getSize();
            Serial.printf("[OTA] Firmware size: %d bytes\n", contentLength);

            if (Update.begin(contentLength)) {
                Serial.println("[OTA] Writing firmware...");
                size_t written = Update.writeStream(*http.getStreamPtr());

                Serial.printf("[OTA] Written %d/%d bytes\n", (int)written,
                              contentLength);

                if (Update.end()) {
                    if (Update.isFinished()) {
                        Serial.println("[OTA] Update finished, restarting...");
                        ESP.restart();
                    } else {
                        Serial.println(
                            "[OTA] Update not finished, something went wrong.");
                    }
                } else {
                    Serial.printf("[OTA] Update error: %d\n",
                                  Update.getError());
                }
            } else {
                Serial.println("[OTA] Not enough space for OTA.");
            }
        } else {
            Serial.printf("[OTA] HTTP GET failed, code: %d\n", httpCode);
        }
        http.end();
    } else {
        Serial.println("[OTA] Unable to connect to update server!");
    }
}

void meshInit() {
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);

    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA, 6);
    mesh.onReceive(&receivedCallback);
}

void receivedCallback(const uint32_t& from, const String& msg) {
    Serial.printf("bridge: Received from %u msg=%s\n", from, msg.c_str());

    if (msg == "U") {
        setLedColor(0, 0, 255);
        performFirmwareUpdate();
        return;
    }

    rootId = msg.toInt();
    Serial.print("New root ID: ");
    Serial.println(rootId);
}

void setup() {
    Serial.begin(115200);

    pixels.begin();
    pixels.setBrightness(5);

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