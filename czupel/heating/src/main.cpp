#include <ArduinoJson.h>
#include <AutoPID.h>
#include <DallasTemperature.h>
#include <EEPROM.h>
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

#define PIN_OUTPUT D1
#define ONE_WIRE_BUS D2

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
int numberOfDevices;

DeviceAddress devaddr_cold = {0x28, 0x4B, 0x69, 0xE0, 0x00, 0x00, 0x00, 0x38};
DeviceAddress devaddr_mixed = {0x28, 0x3C, 0x06, 0xE0, 0x00, 0x00, 0x00, 0x5B};
DeviceAddress devaddr_hot = {0x28, 0x6C, 0x4D, 0xDC, 0x00, 0x00, 0x00, 0xB6};

SimpleTimer timer;

double temp_mixed, temp_cold, temp_hot, output;

struct PidSettings {
    double target;
    double kp;
    double kd;
    double ki;
    double integral;
};

struct PidSettings pid_settings;

AutoPID myPID(&temp_mixed, &(pid_settings.target), &output, 0, 255,
              pid_settings.kp, pid_settings.ki, pid_settings.kd);

char out[256];

void callback(char *topic, uint8_t *payload, int length) {
    string message = "";
    for (int i = 1; i < length; i++) {
        message += (char)payload[i];
    }
    switch ((char)payload[0]) {
        case 'p':
            pid_settings.kp = stof(message);
            break;
        case 'i':
            pid_settings.ki = stof(message);
            break;
        case 'd':
            pid_settings.kd = stof(message);
            break;
        case 't':
            pid_settings.target = stof(message);
            break;
        case 'I':
            myPID.setIntegral(stof(message));
            break;
        default:
            return;
    }
    myPID.setGains(pid_settings.kp, pid_settings.ki, pid_settings.kd);
    EEPROM.put(0, pid_settings);
    EEPROM.commit();
}

void wifi_connect() {
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.println("Connecting to WiFi..");
    }
    Serial.println(WiFi.localIP());
}

void mqtt_connect() {
    client.setServer(mqtt_broker, mqtt_port);
    client.setCallback(callback);
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
}

void send_temp() {
    DynamicJsonDocument data(1024);
    temp_cold = sensors.getTempC(devaddr_cold);
    temp_mixed = sensors.getTempC(devaddr_mixed);
    temp_hot = sensors.getTempC(devaddr_hot);
    if (temp_hot > pid_settings.target) {
        myPID.run();
    } else {
        output = 255;
    }
    analogWrite(PIN_OUTPUT, (int)output);
    pid_settings.integral = myPID.getIntegral();
    data["cold"] = temp_cold;
    data["mixed"] = temp_mixed;
    data["hot"] = temp_hot;
    data["target"] = pid_settings.target;
    data["integral"] = pid_settings.integral;
    data["pid_output"] = output;
    data["kp"] = pid_settings.kp;
    data["ki"] = pid_settings.ki;
    data["kd"] = pid_settings.kd;
    serializeJson(data, out);
    serializeJsonPretty(data, Serial);
    client.publish("/heating/metrics", out);
    sensors.requestTemperatures();
}

void setup() {
    EEPROM.begin(sizeof(PidSettings));
    EEPROM.get(0, pid_settings);
    myPID.setGains(pid_settings.kp, pid_settings.ki, pid_settings.kd);
    Serial.begin(115200);
    wifi_connect();
    mqtt_connect();

    sensors.begin();
    numberOfDevices = sensors.getDeviceCount();
    Serial.print("Locating devices...");
    Serial.print("Found ");
    Serial.print(numberOfDevices, DEC);
    Serial.println(" devices.");
    sensors.setResolution(devaddr_cold, 12);
    sensors.setResolution(devaddr_mixed, 12);
    sensors.setResolution(devaddr_hot, 12);
    sensors.requestTemperatures();
    pinMode(PIN_OUTPUT, OUTPUT);
    timer.setInterval(750, send_temp);
    myPID.setTimeStep(750);
}

void loop() {
    if (!client.connected()) {
        mqtt_connect();
    }
    client.loop();
    timer.run();
}
