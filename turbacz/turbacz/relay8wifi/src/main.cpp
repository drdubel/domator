#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <credentials.h>

#define USE_BOARD 75
#define NO_PWMPIN
const byte wifiActivityPin = 255;

#define NLIGHTS 8

char msg[2];
const char *mqtt_broker = "192.168.3.244";
const int mqtt_port = 1883;
const char *mqttUser = "relay1";

WiFiClient espClient;
PubSubClient client(espClient);

int relays[NLIGHTS] = {32, 33, 25, 26, 27, 14, 12, 13};
int lights[NLIGHTS] = {0, 0, 0, 0, 0, 0, 0, 0};
char whichLight;

void callback(char *topic, uint8_t *payload, int length);

void wifiConnect() {
    Serial.print("Connecting to ");
    Serial.print(ssid);

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(500);
    }

    Serial.println();
}

void mqttConnect() {
    client.setServer(mqtt_broker, mqtt_port);
    client.setCallback(callback);
    while (!client.connected()) {
        if (client.connect(mqttUser)) {
        } else {
            delay(2000);
        }
    }

    client.subscribe("/switch/1/state");
}

void callback(char *topic, uint8_t *payload, int length) {
    if ((length == 1) && ((char)payload[0] == 'S')) {
        for (int i = 0; i < NLIGHTS; ++i) {
            msg[0] = 'A' + i;
            msg[1] = lights[i] ? '1' : '0';
            client.publish("/switch/1/state", msg);
        }
    } else if (payload[0] >= 'a' && payload[0] < 'a' + NLIGHTS && length == 2) {
        Serial.print("Received message: ");
        Serial.print(topic);
        Serial.print(" with payload: ");
        for (int i = 0; i < length; ++i) {
            Serial.print((char)payload[i]);
        }
        Serial.println();
        whichLight = (char)payload[0] - 'a';
        lights[whichLight] = (char)payload[1] - '0';

        digitalWrite(relays[whichLight], lights[whichLight] ? HIGH : LOW);
    }
}

void setup() {
    Serial.begin(115200);

    wifiConnect();
    mqttConnect();

    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(relays[i], OUTPUT);
        digitalWrite(relays[i], LOW);
    }
}

void loop() {
    if (client.connected()) {
        Serial.println("Client connected");
        client.loop();
    } else {
        mqttConnect();
    }
}
