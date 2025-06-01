#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <credentials.h>

#define NLIGHTS 3

using namespace std;

char msg[2];
const char *mqtt_broker = "192.168.3.244";
const int mqtt_port = 1883;
const char *mqttUser = "turbacz";

WiFiClient espClient;
PubSubClient client(espClient);

const int buttonPins[NLIGHTS] = {A2, A3, A4};
int lights[NLIGHTS] = {0, 0, 0};
char whichLight;
int lastButtonState = HIGH;

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
        if (client.connect("lights-wifi")) {
        } else {
            delay(2000);
        }
    }
    client.subscribe("/switch/1/cmd");
    client.publish("/switch/1/state", "jest");
}

void callback(char *topic, uint8_t *payload, int length) {
    if ((length == 1) && ((char)payload[0] == 'S')) {
        for (int i = 0; i < NLIGHTS; ++i) {
            Serial.write('A' + i);
        }
    } else {
        whichLight = (char)payload[0] - 'A';
        lights[whichLight] = (char)payload[1] - '0';
    }
}

void setup() {
    Serial.begin(115200);
    wifiConnect();
    mqttConnect();

    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(buttonPins[i], INPUT_PULLUP);
    }
}

void loop() {
    for (int i = 0; i < NLIGHTS; i++) {
        int currentState = digitalRead(buttonPins[i]);

        if (currentState == LOW && lastButtonState == HIGH) {
            lights[i] = !lights[i];

            msg[0] = 'a' + i;
            msg[1] = lights[i] ? '1' : '0';
            client.publish("/switch/1/state", msg);

            delay(250);
        }

        lastButtonState = currentState;
    }

    if (client.connected()) {
        Serial.println("Client connected");
        client.loop();
    } else {
        mqttConnect();
    }
}
