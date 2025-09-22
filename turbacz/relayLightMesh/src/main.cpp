#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <credentials.h>
#include <painlessMesh.h>

#define USE_BOARD 75
#define NO_PWMPIN
const byte wifiActivityPin = 255;

#define NLIGHTS 8

void receivedCallback(const uint32_t& from, const String& msg);

IPAddress myIP(0, 0, 0, 0);
AsyncWebServer server(80);
painlessMesh mesh;

uint32_t rootId;

int relays[NLIGHTS] = {32, 33, 25, 26, 27, 14, 12, 13};
int lights[NLIGHTS] = {0, 0, 0, 0, 0, 0, 0, 0};
char whichLight;

const char* firmware_url = "https://czupel.dry.pl/static/data/relay8wifi.bin";

char message[2];

void performFirmwareUpdate() {
    Serial.println("[OTA] Stopping mesh...");
    mesh.stop();
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
    client.setInsecure();
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

    Serial.printf("Mesh started with ID %u\n", mesh.getNodeId());
}

void receivedCallback(const uint32_t& from, const String& msg) {
    Serial.printf("bridge: Received from %u msg=%s\n", from, msg.c_str());

    if (msg == "S") {
        for (int i = 0; i < NLIGHTS; ++i) {
            message[0] = 'A' + i;
            message[1] = lights[i] ? '1' : '0';

            mesh.sendSingle(from, String(message, 2));
        }
    } else if (msg[0] >= 'a' && msg[0] < 'a' + NLIGHTS && msg.length() == 2) {
        whichLight = (char)msg[0] - 'a';
        lights[whichLight] = (char)msg[1] - '0';

        digitalWrite(relays[whichLight], lights[whichLight] ? HIGH : LOW);

        message[0] = 'A' + whichLight;
        message[1] = lights[whichLight] ? '1' : '0';
        mesh.sendSingle(from, String(message, 2));

        Serial.print("Light ");
        Serial.print('a' + whichLight);
        Serial.print(" set to ");
        Serial.println(lights[whichLight] ? "ON" : "OFF");
    } else if (msg == "U") {
        performFirmwareUpdate();
    } else {
        rootId = msg.toInt();
        Serial.print("New root ID: ");
        Serial.println(rootId);

        mesh.sendSingle(from, "R");
    }
}

void setup() {
    Serial.begin(115200);

    delay(2000);
    meshInit();

    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(relays[i], OUTPUT);
        digitalWrite(relays[i], LOW);
    }

    pinMode(23, OUTPUT);
    digitalWrite(23, LOW);
}

void loop() { mesh.update(); }