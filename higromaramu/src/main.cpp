#include <AHT20.h>
#include <Adafruit_BMP280.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <credentials.h>

#include <list>

using namespace std;

AHT20 aht20;
Adafruit_BMP280 bmp;

list<float> temperatures, humidities, pressures;
int i = 0, interval = 2000, samples = 15;
long long startTime;

float average(list<float> &values) {
    float sum = 0;
    for (auto &value : values) sum += value;

    return sum / values.size();
}

void sendData() {
    String temperature = String(average(temperatures), 2);
    String humidity = String(average(humidities), 2);
    String pressure = String(average(pressures), 2);
    String payload =
        "measurement,dev=wemosS2mini,location=LadyTwin temperature=" +
        temperature + ",pressure=" + pressure + ",humidity=" + humidity;

    Serial.println("Average temperature: " + temperature + " C\t");
    Serial.println("Average humidity: " + humidity + "% RH");
    Serial.println("Average pressure: " + pressure + " hPa");

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

    if (aht20.begin() == false) {
        Serial.println("AHT20 not detected. Please check wiring. Freezing.");
        while (1);
    }
    Serial.println("AHT20 acknowledged.");

    if (!bmp.begin()) {
        Serial.println("BMP280 not detected. Please check wiring. Freezing.");
        while (1);
    }
    Serial.println("BMP280 acknowledged.");
}

void setup() {
    Serial.begin(115200);
    // while (!Serial);  // DEBUG

    initWiFi();
    initSensors();

    startTime = millis();
}

void loop() {
    if (millis() - startTime >= i * interval) {
        float temperature = aht20.getTemperature();
        float humidity = aht20.getHumidity();
        float pressure = bmp.readPressure() / 100;

        if (temperatures.size() >= samples) temperatures.pop_front();
        if (humidities.size() >= samples) humidities.pop_front();
        if (pressures.size() >= samples) pressures.pop_front();

        temperatures.push_back(temperature);
        humidities.push_back(humidity);
        pressures.push_back(pressure);

        i++;
    }

    if (millis() - startTime >= interval * samples) {
        startTime = millis();
        i = 0;

        sendData();
    }
}