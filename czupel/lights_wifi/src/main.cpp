#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
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
        Serial.println("Connecting to WiFi..");
    }
    Serial.println(WiFi.localIP());
}

void mqttConnect() {
    client.setServer(mqtt_broker, mqtt_port);
    client.setCallback(callback);
    while (!client.connected()) {
        Serial.printf(
            "\nThe client lights-wifi connects to the public mqtt broker\n");
        if (client.connect("lights-wifi", mqttUser, mqttPassword)) {
            Serial.println("Public emqx mqtt broker connected");
        } else {
            Serial.print("failed with state ");
            Serial.print(client.state());
            delay(2000);
        }
    }
    client.subscribe("/switch/1/cmd");
}

void getCmd(char cmd_char) {
    if (('a' <= cmd_char) && (cmd_char <= 'l')) {
        whichLight = cmd_char;
    } else {
        msg[0] = whichLight;
        msg[1] = cmd_char;
        client.publish("/switch/1/state", msg);
        Serial.println(msg);
    }
}

void callback(char *topic, uint8_t *payload, int length) {
    Serial.println("-----------------------");
    Serial.print("Message arrived in topic: ");
    Serial.println(topic);
    Serial.print("Message: ");
    if ((length == 1) && ((char)payload[0] == 'S')) {
        for (int i = 0; i < NLAMPS; ++i) {
            Serial2.write('A' + i);
        };
        Serial.println();
    } else {
        Serial2.write((char)payload[0]);
        Serial2.write((char)payload[1]);
        Serial.println();
    }
}

void setup() {
    Serial.begin(115200);
    Serial2.begin(115200);
    wifiConnect();
    mqttConnect();
}

void loop() {
    if (!client.connected()) {
        mqttConnect();
    }
    while (Serial2.available() > 0) {
        getCmd(Serial2.read());
    }
    client.loop();
}