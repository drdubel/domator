#include <AHT20.h>
#include <Adafruit_BMP280.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <credentials.h>

#include <list>

using namespace std;

const int VoltagePin = 1;
const char* signalkClientId = "higromaramu-esp32";

AHT20 aht20;
Adafruit_BMP280 bmp;
Preferences preferences;
WebSocketsClient webSocket;
String signalkToken;
bool signalkConnected = false;
float temperature_sum = 0, humidity_sum = 0, pressure_sum = 0, voltage_sum = 0;
int i = 0, j = 0, interval = 2000, samples = 10, vol_interval = 40;
long long startTime;

float average(list<float>& values) {
    float sum = 0;
    for (auto& value : values) sum += value;

    return sum / values.size();
}

String signalkBaseUrl() {
    return "http://" + String(signalkHost) + ":" + String(signalkPort);
}

bool requestSignalKAccessToken() {
    HTTPClient http;
    http.begin(signalkBaseUrl() + "/signalk/v1/access/requests");
    http.addHeader("Content-Type", "application/json");

    String payload = "{\"clientId\":\"" + String(signalkClientId) +
                     "\",\"description\":\"Higromaramu ESP32 sensor\"}";
    int httpCode = http.POST(payload);
    if (httpCode <= 0) {
        Serial.println("Access request failed: " +
                       String(http.errorToString(httpCode)));
        http.end();
        return false;
    }

    String response = http.getString();
    http.end();

    int hrefStart = response.indexOf("\"href\":\"");
    if (hrefStart < 0) {
        Serial.println("Access request response missing href: " + response);
        return false;
    }
    hrefStart += 8;
    int hrefEnd = response.indexOf('"', hrefStart);
    String href = response.substring(hrefStart, hrefEnd);

    Serial.println(
        "Approve this device in SignalK admin UI (Security > Access "
        "Requests), then it will keep polling: " +
        href);

    while (true) {
        delay(5000);

        HTTPClient pollHttp;
        pollHttp.begin(signalkBaseUrl() + href);
        int pollCode = pollHttp.GET();
        if (pollCode <= 0) {
            pollHttp.end();
            continue;
        }

        String pollResponse = pollHttp.getString();
        pollHttp.end();

        if (pollResponse.indexOf("\"state\":\"PENDING\"") >= 0) {
            Serial.println("Still waiting for approval...");
            continue;
        }

        int tokenStart = pollResponse.indexOf("\"token\":\"");
        if (tokenStart < 0) {
            Serial.println("Access request denied or errored: " + pollResponse);
            return false;
        }
        tokenStart += 9;
        int tokenEnd = pollResponse.indexOf('"', tokenStart);
        signalkToken = pollResponse.substring(tokenStart, tokenEnd);

        preferences.putString("sk_token", signalkToken);
        Serial.println("SignalK access token approved and saved.");
        return true;
    }
}

void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            Serial.println("SignalK WebSocket connected.");
            signalkConnected = true;
            break;
        case WStype_DISCONNECTED:
            Serial.println("SignalK WebSocket disconnected.");
            signalkConnected = false;
            break;
        case WStype_ERROR:
            Serial.println("SignalK WebSocket error.");
            break;
        default:
            break;
    }
}

void initSignalKWebSocket() {
    String path = "/signalk/v1/stream?subscribe=none";
    if (!signalkToken.isEmpty()) {
        path += "&token=" + signalkToken;
    }

    webSocket.begin(signalkHost, signalkPort, path);
    webSocket.onEvent(onWebSocketEvent);
    webSocket.setReconnectInterval(5000);
}

void sendSignalKDelta(const String& path, float value) {
    if (!signalkConnected) {
        Serial.println(path + " -> SignalK WebSocket not connected, skipping");
        return;
    }

    String delta = "{\"updates\":[{\"values\":[{\"path\":\"" + path +
                   "\",\"value\":" + String(value, 4) + "}]}]}";

    webSocket.sendTXT(delta);
}

void sendData() {
    float temperature = temperature_sum / i;
    float humidity = humidity_sum / i;
    float pressure = pressure_sum / i;
    float voltage = voltage_sum / j / 238.875;

    Serial.println("Average temperature: " + String(temperature, 2) + " C\t");
    Serial.println("Average humidity: " + String(humidity, 2) + "% RH");
    Serial.println("Average pressure: " + String(pressure, 2) + " hPa");
    Serial.println("Average voltage: " + String(voltage, 2) + " V");

    sendSignalKDelta("environment.inside.temperature", temperature + 273.15);
    sendSignalKDelta("environment.inside.humidity", humidity / 100);
    sendSignalKDelta("environment.inside.pressure", pressure * 100);
    sendSignalKDelta("electrical.batteries.0.voltage", voltage);
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

    preferences.begin("higromaramu", false);
    signalkToken = preferences.getString("sk_token", "");
    if (signalkToken.isEmpty()) {
        Serial.println("No stored SignalK token, requesting access...");
        requestSignalKAccessToken();
    }

    initSignalKWebSocket();

    analogSetAttenuation(ADC_2_5db);
    analogReadMilliVolts(VoltagePin);

    startTime = millis();
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi connection lost. Reconnecting...");

        initWiFi();
    }

    webSocket.loop();

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