#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <credentials.h>

#define NLAMPS 12

using namespace std;

char msg[2];
const char *mqtt_broker = "192.168.3.10";
const int mqtt_port = 1883;
const char *mqttUser = "switch1-wifi";

WiFiClient espClient;
PubSubClient client(espClient);

char whichLight;

void callback(char *topic, uint8_t *payload, int length);

void wifiConnect() {
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }
}

void mqttConnect() {
    client.setServer(mqtt_broker, mqtt_port);
    client.setCallback(callback);
    while (!client.connected()) {
        if (client.connect("lights-wifi", mqttUser, mqttPassword)) {
        } else {
            delay(2000);
        }
    }
    client.subscribe("/switch/1/cmd");
    client.publish("/switch/1/state", "jest");
}

void getCmd(char cmd_char) {
    if (('a' <= cmd_char) && (cmd_char <= 'l')) {
        whichLight = cmd_char;
    } else {
        msg[0] = whichLight;
        msg[1] = cmd_char;
        client.publish("/switch/1/state", msg);
    }
}

void callback(char *topic, uint8_t *payload, int length) {
    if ((length == 1) && ((char)payload[0] == 'S')) {
        for (int i = 0; i < NLAMPS; ++i) {
            Serial.write('A' + i);
        }
    } else {
        Serial.write((char)payload[0]);
        Serial.write((char)payload[1]);
    }
}

void setup() {
    Serial.begin(115200);
    wifiConnect();
    mqttConnect();
}

void loop() {
    if (!client.connected()) {
        mqttConnect();
    }
    while (Serial.available() > 0) {
        getCmd(Serial.read());
    }
    client.loop();
}