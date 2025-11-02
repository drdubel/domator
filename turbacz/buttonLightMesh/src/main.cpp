#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <credentials.h>
#include <painlessMesh.h>

#define NLIGHTS 7
#define LED_PIN 8
#define NUM_LEDS 1

void mqttCallback(char* topic, byte* payload, unsigned int length);

IPAddress mqtt_broker(192, 168, 3, 10);
const int mqtt_port = 1883;

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

IPAddress myIP(0, 0, 0, 0);
AsyncWebServer server(80);

const char* DEVICE_ID = "1234";

WiFiClient wifiClient;
PubSubClient mqttClient(mqtt_broker, mqtt_port, wifiClient);

const int buttonPins[NLIGHTS] = {A0, A1, A3, A4, A5, 6, 7};
int lastTimeClick[NLIGHTS];
int debounceDelay = 250;
char whichLight;
int lastButtonState[NLIGHTS] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};

const char* firmware_url =
    "https://czupel.dry.pl/static/data/wifiLightButton.bin";

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
    Serial.println("[OTA] Switching to STA mode...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(SSID, PASSWORD);

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

void setupWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(SSID, PASSWORD);

    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println(" connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
}

void mqttConnect() {
    Serial.print("Connecting to MQTT broker at ");
    Serial.print(mqtt_broker);
    Serial.print(" with user ");
    Serial.println(MQTT_USER);
    mqttClient.setCallback(mqttCallback);

    while (!mqttClient.connected()) {
        if (mqttClient.connect(DEVICE_ID, MQTT_USER, MQTT_PASSWORD)) {
            Serial.println("Connected to MQTT broker");
            digitalWrite(LED_BUILTIN, HIGH);
        } else {
            static unsigned long lastToggle = 0;
            if (millis() - lastToggle > 500) {
                digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
                lastToggle = millis();
            }
        }
    }

    String topic = "/switch/" + String(DEVICE_ID);
    mqttClient.publish(topic.c_str(), "connected");
    mqttClient.subscribe("/switch/cmd");

    setLedColor(0, 255, 0);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg;
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    Serial.printf("MQTT: Received message on %s: %s\n", topic, msg.c_str());

    if (msg == "U") {
        setLedColor(0, 0, 255);
        performFirmwareUpdate();
        return;
    }
}

void setup() {
    Serial.begin(115200);

    pixels.begin();
    pixels.setBrightness(5);

    Serial.println(DEVICE_ID);

    setupWiFi();
    mqttConnect();

    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(buttonPins[i], INPUT_PULLDOWN);
    }
}

void loop() {
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

            String topic = "/switch/" + String(DEVICE_ID);
            mqttClient.publish(topic.c_str(), String(msg).c_str());
        }

        lastButtonState[i] = currentState;
    }

    if (!mqttClient.connected()) {
        mqttConnect();
    } else {
        mqttClient.loop();
    }
}