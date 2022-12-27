#include <ArduinoJson.h>
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

double temp_mixed, temp_cold, temp_hot, output, integral,
    temp_target = 30, Kp = 200, Ki = 0.1, Kd = 350;

AutoPID myPID(&temp_mixed, &temp_target, &output, 0, 255, Kp, Ki, Kd);

char out[128];

void printAddress(DeviceAddress deviceAddress) {
    for (uint8_t i = 0; i < 8; i++) {
        if (deviceAddress[i] < 16) Serial.print("0");
        Serial.print(deviceAddress[i], HEX);
    }
}

void callback(char *topic, uint8_t *payload, int length) {
    string message = "";
    for (int i = 1; i < length; i++) {
        message += (char)payload[i];
    }
    if ((char)payload[0] == 'p') {
        Kp = stof(message);
    }
    if ((char)payload[0] == 'i') {
        Ki = stof(message);
    }
    if ((char)payload[0] == 'd') {
        Kd = stof(message);
    }
    if ((char)payload[0] == 't') {
        temp_target = stof(message);
    }
}

void send_temp() {
    DynamicJsonDocument data(1024);
    temp_cold = sensors.getTempCByIndex(0);
    temp_hot = sensors.getTempCByIndex(1);
    temp_mixed = sensors.getTempCByIndex(2);
    if (temp_hot > temp_target) {
        myPID.run();
    }
    integral = myPID.getIntegral();
    data["cold"] = temp_cold;
    data["mixed"] = temp_mixed;
    data["hot"] = temp_hot;
    data["target"] = temp_target;
    data["integral"] = integral;
    data["pid_output"] = output;
    data["kp"] = Kp;
    data["ki"] = Ki;
    data["kd"] = Kd;
    serializeJson(data, out);
    client.publish("/heating/metrics", out);
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
            "\nThe client blinds-wifi connects to the public mqtt "
            "broker\n");
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
