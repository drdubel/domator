#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <credentials.h>

#define USE_BOARD 75
#define NO_PWMPIN
const byte wifiActivityPin = 255;

#define NLIGHTS 8

char msg[2];
const char *mqtt_broker = "10.42.0.1";
const int mqtt_port = 1883;
const char *mqttUser = "relay" DEVICE_ID "-wifi";

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

    Serial.println(WiFi.localIP());
    Serial.println();
}

void mqttConnect() {
    Serial.print("Connecting to MQTT broker at ");
    Serial.print(mqtt_broker);
    Serial.print(" with user ");
    Serial.println(mqttUser);

    client.setCallback(callback);
    client.setServer(mqtt_broker, mqtt_port);

    while (!client.connected()) {
        if (client.connect(mqttUser, mqttUser, mqttPassword)) {
            Serial.println("Connected to MQTT broker");
        } else {
            delay(2000);
        }
    }

    client.subscribe("/relay/" DEVICE_ID "/cmd");
}

void callback(char *topic, uint8_t *payload, int length) {
    if ((length == 1) && ((char)payload[0] == 'S')) {
        for (int i = 0; i < NLIGHTS; ++i) {
            msg[0] = 'A' + i;
            msg[1] = lights[i] ? '1' : '0';

            client.publish("/relay/" DEVICE_ID "/state", msg);
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

        msg[0] = 'A' + whichLight;
        msg[1] = lights[whichLight] ? '1' : '0';
        client.publish("/relay/" DEVICE_ID "/state", msg);

        Serial.print("Light ");
        Serial.print('a' + whichLight);
        Serial.print(" set to ");
        Serial.println(lights[whichLight] ? "ON" : "OFF");
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
        client.loop();
    } else {
        mqttConnect();
    }
}
