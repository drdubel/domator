#include <AHT20.h>
#include <Adafruit_BMP280.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <credentials.h>

#include <list>

using namespace std;

const int VoltagePin = 1;

AHT20 aht20;
Adafruit_BMP280 bmp;
float temperature_sum = 0, humidity_sum = 0, pressure_sum = 0, voltage_sum = 0;
int i = 0, j = 0, interval = 2000, samples = 10, vol_interval = 40;
long long startTime;

float average(list<float> &values) {
    float sum = 0;
    for (auto &value : values) sum += value;

    return sum / values.size();
}

void sendData() {
    String temperature = String(temperature_sum / i, 2);
    String humidity = String(humidity_sum / i, 2);
    String pressure = String(pressure_sum / i, 2);
    String voltage = String(voltage_sum / j / 238.875, 2);
    String payload =
        "measurement,dev=wemosS2mini,location=LadyTwin temperature=" +
        temperature + ",pressure=" + pressure + ",humidity=" + humidity +
        ",voltage=" + voltage;

    Serial.println("Average temperature: " + temperature + " C\t");
    Serial.println("Average humidity: " + humidity + "% RH");
    Serial.println("Average pressure: " + pressure + " hPa");
    Serial.println("Average voltage: " + voltage + " V");

    HTTPClient http;
    http.begin("metrics.dry.pl", 443, "/api/v2/write", test_root_ca,
               test_client_cert, test_client_key);
    int httpCode = http.POST(payload);
    if (httpCode > 0) {
        Serial.println("HTTP Response code: " + httpCode);
    }

    else {
        Serial.println("Error on HTTP request: " + httpCode);
    }
}

void initWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    Serial.print("Connecting to WiFi");

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(500);
    }
    Serial.println("");
    Serial.println(WiFi.localIP());
}

void initSensors() {
    Wire.begin();

    if (!aht20.begin()) {
        Serial.println("AHT20 not detected. Please check wiring.");
        while (1);
    }
    Serial.println("AHT20 acknowledged.");

    if (!bmp.begin()) {
        Serial.println("BMP280 not detected. Please check wiring.");
        while (1);
    }
    Serial.println("BMP280 acknowledged.");
}

void setup() {
    Serial.begin(115200);
    // while (!Serial);  // DEBUG

    initWiFi();
    initSensors();

    analogSetAttenuation(ADC_2_5db);
    analogReadMilliVolts(VoltagePin);

    startTime = millis();
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi connection lost. Reconnecting...");

        initWiFi();
    }

    if (millis() - startTime >= j * vol_interval) {
        float voltage = analogRead(VoltagePin);

        voltage_sum += voltage;

        j++;
    }

    if (millis() - startTime >= i * interval) {
        float temperature = aht20.getTemperature();
        float humidity = aht20.getHumidity();
        float pressure = bmp.readPressure() / 100;

        temperature_sum += temperature;
        humidity_sum += humidity;
        pressure_sum += pressure;

        i++;
    }

    if (millis() - startTime >= interval * samples) {
        startTime = millis();

        sendData();

        i = 0;
        j = 0;
        temperature_sum = 0;
        humidity_sum = 0;
        pressure_sum = 0;
        voltage_sum = 0;
    }
}