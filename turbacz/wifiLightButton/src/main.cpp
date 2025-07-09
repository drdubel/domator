#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <credentials.h>

#define NLIGHTS 7

using namespace std;

char msg;
const char *mqtt_broker = "10.42.0.1";
const int mqtt_port = 1883;
const char *mqttUser = "switch" DEVICE_ID "-wifi";

WiFiClient espClient;
PubSubClient client(espClient);

const int buttonPins[NLIGHTS] = {A0, A1, A3, A4, A5, 6, 7};
int lastTimeClick[NLIGHTS];
int debounceDelay = 250;
char whichLight;
int lastButtonState[NLIGHTS] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};

void wifiConnect() {
    Serial.print("Connecting to ");
    Serial.print(ssid);

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(500);
    }

    Serial.println(WiFi.localIP());
    Serial.println();
}

void mqttConnect() {
    Serial.print("Connecting to MQTT broker at ");
    Serial.print(mqtt_broker);
    Serial.print(" with user ");
    Serial.println(mqttUser);

    client.setServer(mqtt_broker, mqtt_port);
    while (!client.connected()) {
        if (client.connect(mqttUser, mqttUser, mqttPassword)) {
            Serial.println("Connected to MQTT broker");
        } else {
            delay(2000);
        }
    }
}

void setup() {
    Serial.begin(115200);

    wifiConnect();
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
            client.publish("/switch/" DEVICE_ID, String(msg).c_str());
        }

        lastButtonState[i] = currentState;
    }

    if (client.connected()) {
        client.loop();
    } else {
        mqttConnect();
    }
}
