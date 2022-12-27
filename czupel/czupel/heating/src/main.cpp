#include <AutoPID.h>
#include <DallasTemperature.h>
#include <ESP8266WiFi.h>
#include <OneWire.h>
#include <PubSubClient.h>
#include <SimpleTimer.h>
#include <credentials.h>

#include <string>

using namespace std;

const char *mqtt_broker = "192.168.3.10";
const int mqtt_port = 1883;
const char *mqttUser = "heating-wifi";

WiFiClient espClient;
PubSubClient client(espClient);

#define PIN_OUTPUT 3
#define ONE_WIRE_BUS 4

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
int numberOfDevices;

DeviceAddress tempDeviceAddress;

SimpleTimer timer;

double temp_mixed, temp_cool, temp_hot, output, integral,
    target_temperature = 30, Kp = 200, Ki = 0.1, Kd = 350;

AutoPID myPID(&temp_mixed, &target_temperature, &output, 0, 255, Kp, Ki, Kd);

void printAddress(DeviceAddress deviceAddress) {
    for (uint8_t i = 0; i < 8; i++) {
        if (deviceAddress[i] < 16) Serial.print("0");
        Serial.print(deviceAddress[i], HEX);
    }
}

void callback(char *topic, uint8_t *payload, int length) {
    string message = "";
    for (int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    if (message[0] == 'K') {
        switch (message[1]) {
            case 'p':
                Kp = 0;
                for (int i = 3; i < length; i++) {
                    Kp += (int)message[i] * pow(10, length - (i + 1));
                }
                break;
            case 'i':
                Ki = 0;
                for (int i = 3; i < length; i++) {
                    Ki += (int)message[i] * pow(10, length - (i + 1));
                }
                break;
            case 'd':
                Kd = 0;
                for (int i = 3; i < length; i++) {
                    Kd += (int)message[i] * pow(10, length - (i + 1));
                }
                break;
        }
    } else {
        target_temperature = stof(message);
    }
}

void send_temp() {
    temp_cool = sensors.getTempCByIndex(0);
    temp_hot = sensors.getTempCByIndex(1);
    temp_mixed = sensors.getTempCByIndex(2);
    myPID.run();
    integral = myPID.getIntegral();
    Serial.println(&("cold " + to_string(temp_cool))[0]);
    Serial.println(&("mixed " + to_string(temp_mixed))[0]);
    Serial.println(&("hot " + to_string(temp_hot))[0]);
    Serial.println(&("pid " + to_string(output))[0]);
    Serial.println(&("integral " + to_string(integral))[0]);
    client.publish("/heating/metrics", &("cold " + to_string(temp_cool))[0]);
    client.publish("/heating/metrics", &("mixed " + to_string(temp_mixed))[0]);
    client.publish("/heating/metrics", &("hot " + to_string(temp_hot))[0]);
    client.publish("/heating/metrics",
                   &("target " + to_string(target_temperature))[0]);
    client.publish("/heating/metrics", &("integral " + to_string(integral))[0]);
    client.publish("/heating/metrics", &("pid_output " + to_string(output))[0]);
    sensors.requestTemperatures();
}

void setup() {
    Serial.begin(115200);

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.println("Connecting to WiFi..");
    }
    client.setServer(mqtt_broker, mqtt_port);
    client.setCallback(callback);
    Serial.println(WiFi.localIP());
    while (!client.connected()) {
        Serial.printf(
            "\nThe client blinds-wifi connects to the public mqtt broker\n");
        if (client.connect("blinds-wifi", mqttUser, mqttPassword)) {
            Serial.println("Public emqx mqtt broker connected");
        } else {
            Serial.print("failed with state ");
            Serial.print(client.state());
            delay(2000);
        }
    }
    client.subscribe("/heating/cmd");

    sensors.begin();
    numberOfDevices = sensors.getDeviceCount();
    Serial.print("Locating devices...");
    Serial.print("Found ");
    Serial.print(numberOfDevices, DEC);
    Serial.println(" devices.");
    for (int i = 0; i < numberOfDevices; i++) {
        if (sensors.getAddress(tempDeviceAddress, i)) {
            Serial.print("Found device ");
            Serial.print(i, DEC);
            Serial.print(" with address: ");
            printAddress(tempDeviceAddress);
            Serial.println();
        } else {
            Serial.print("Found ghost device at ");
            Serial.print(i, DEC);
            Serial.print(
                " but could not detect address. Check power and cabling");
        }
    }
    sensors.getAddress(tempDeviceAddress, 0);
    sensors.setResolution(tempDeviceAddress, 12);
    sensors.getAddress(tempDeviceAddress, 1);
    sensors.setResolution(tempDeviceAddress, 12);
    sensors.getAddress(tempDeviceAddress, 2);
    sensors.setResolution(tempDeviceAddress, 12);
    sensors.requestTemperatures();
    timer.setInterval(750, send_temp);
    myPID.setTimeStep(750);
}

void loop() {
    client.loop();
    timer.run();
}
