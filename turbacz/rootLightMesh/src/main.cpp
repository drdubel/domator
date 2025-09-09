#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <credentials.h>
#include <painlessMesh.h>
#include <webpage.h>

#include <algorithm>
#include <vector>

#define HOSTNAME "ROOT_Node"

#define NLIGHTS 7
#define LED_PIN 8
#define NUM_LEDS 1

IPAddress mqtt_broker(192, 168, 3, 244);
const int mqtt_port = 1883;
const char* mqttUser = "root_node";

std::vector<uint32_t> nodes;

void receivedCallback(const uint32_t& from, const String& msg);
void droppedConnectionCallback(uint32_t nodeId);
void newConnectionCallback(uint32_t nodeId);

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

IPAddress myIP(0, 0, 0, 0);
AsyncWebServer server(80);
painlessMesh mesh;
WiFiClient wifiClient;
PubSubClient mqttClient(mqtt_broker, mqtt_port, wifiClient);

IPAddress getlocalIP() { return IPAddress(mesh.getStationIP()); }

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
    pixels.setPixelColor(0, pixels.Color(r, g, b));
    pixels.show();
}

// DO NAPRAWY
void updateLedStatus() {
    bool wifi_ok = (WiFi.status() == WL_CONNECTED);

    if (wifi_ok)
        setLedColor(255, 255, 255);
    else
        setLedColor(0, 0, 0);
}

void mqttConnect() {
    Serial.print("Connecting to MQTT broker at ");
    Serial.print(mqtt_broker);
    Serial.print(" with user ");
    Serial.println(mqttUser);

    while (!mqttClient.connected()) {
        if (mqttClient.connect(mqttUser)) {
            Serial.println("Connected to MQTT broker");
        } else {
            setLedColor(0, 255, 0);
            delay(500);
            setLedColor(0, 0, 0);
            delay(500);
        }
    }

    mqttClient.publish("/switch/0", "connected");
}

void serverInit() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/html", webpage);
    });
    server.on(
        "/update", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            bool updateSuccess = !Update.hasError();
            AsyncWebServerResponse* response = request->beginResponse(
                200, "text/plain",
                updateSuccess ? "Update successful. Rebooting..."
                              : "Update failed!");
            response->addHeader("Connection", "close");
            request->send(response);

            if (updateSuccess) {
                // Give browser time to read response
                Serial.println("OTA complete, rebooting in 2s...");
                delay(2000);
                ESP.restart();
            }
        },
        [](AsyncWebServerRequest* request, String filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            if (!index) {
                Serial.printf("Update Start: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Update.printError(Serial);
                }
            }
            if (Update.write(data, len) != len) {
                Update.printError(Serial);
            }
            if (final) {
                if (Update.end(true)) {
                    Serial.printf("Update Success: %u bytes\n", index + len);
                } else {
                    Update.printError(Serial);
                }
            }
        });
    server.begin();

    Serial.println("HTTP server started");
}

void meshInit() {
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);

    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA, 6);
    mesh.onReceive(&receivedCallback);
    mesh.onNewConnection(&newConnectionCallback);
    mesh.onDroppedConnection(&droppedConnectionCallback);

    mesh.stationManual(STATION_SSID, STATION_PASSWORD);
    mesh.setHostname(HOSTNAME);

    mesh.setRoot(true);
    mesh.setContainsRoot(true);

    uint32_t rootId = mesh.getNodeId();
    Serial.println("ROOT:" + String(rootId));
}

void droppedConnectionCallback(uint32_t nodeId) {
    auto it = std::find(nodes.begin(), nodes.end(), nodeId);
    if (it != nodes.end()) {
        Serial.print("Node ");
        Serial.print(nodeId);
        Serial.println(" disconnected, removing from nodes map.");
        nodes.erase(it);
    }
}

void newConnectionCallback(uint32_t nodeId) {
    uint32_t rootId = mesh.getNodeId();

    mesh.sendSingle(nodeId, String(rootId));
}

void receivedCallback(const uint32_t& from, const String& msg) {
    Serial.printf("bridge: Received from %u msg=%s\n", from, msg.c_str());

    if (msg[0] >= 97) {
        if (WiFi.status() != WL_CONNECTED) return;
        if (!mqttClient.connected()) mqttConnect();

        String topic = "/switch/" + from;

        Serial.println("Publishing to topic: " + topic);
        mqttClient.publish(topic.c_str(), msg.c_str());
    } else {
        nodes[from] = atoi(msg.c_str());
        Serial.print("Node ");
        Serial.print(from);
        Serial.print(" registered as ");
        Serial.println(msg);
    }
}

void setup() {
    Serial.begin(115200);

    pixels.begin();
    pixels.setBrightness(5);
    setLedColor(0, 0, 0);

    meshInit();
    Serial.printf("MAC Address: %s\n", WiFi.macAddress().c_str());
}

void loop() {
    mesh.update();

    if (myIP != getlocalIP()) {
        Serial.println("Connected to external WiFi!");
        myIP = getlocalIP();
        Serial.println("My IP is " + myIP.toString());

        serverInit();
        mqttConnect();
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (!mqttClient.connected()) mqttConnect();

        mqttClient.loop();
    }
}