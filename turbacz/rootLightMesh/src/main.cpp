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
#include <map>

#define HOSTNAME "ROOT_Node"

#define NLIGHTS 7
#define LED_PIN 8
#define NUM_LEDS 1

IPAddress mqtt_broker(192, 168, 3, 244);
const int mqtt_port = 1883;
const char *mqttUser = "root_node";
uint32_t device_id;

std::map<uint32_t, String> nodes;

void receivedCallback(const uint32_t &from, const String &msg);
void droppedConnectionCallback(uint32_t nodeId);
void newConnectionCallback(uint32_t nodeId);
void mqttCallback(char *topic, byte *payload, unsigned int length);

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

IPAddress myIP(0, 0, 0, 0);
AsyncWebServer server(80);
painlessMesh mesh;
WiFiClient wifiClient;
PubSubClient mqttClient(mqtt_broker, mqtt_port, wifiClient);

static unsigned long lastPrint = 0;

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
    mqttClient.setCallback(mqttCallback);

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

    mqttClient.publish(("/switch/" + String(device_id)).c_str(), "connected");
    mqttClient.subscribe("/switch/cmd");
    mqttClient.subscribe("/relay/cmd/+");
    mqttClient.subscribe("/relay/cmd");
}

void serverInit() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", webpage);
    });

    server.on(
        "/update", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            bool success = !Update.hasError();

            AsyncWebServerResponse *response = request->beginResponse(
                200, "text/plain",
                success ? "Update successful! Rebooting in 5s..."
                        : "Update failed!");
            response->addHeader("Connection", "close");
            request->send(response);

            if (success) {
                Serial.println("OTA update finished, reboot in 5 seconds...");
                xTaskCreate(
                    [](void *param) {
                        delay(5000);
                        ESP.restart();
                    },
                    "rebootTask", 4096, NULL, 1, NULL);
            }
        },
        [](AsyncWebServerRequest *request, String filename, size_t index,
           uint8_t *data, size_t len, bool final) {
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

    device_id = mesh.getNodeId();
    Serial.println("ROOT:" + String(device_id));
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
    String msg;
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    Serial.printf("MQTT: Received message on %s: %s\n", topic, msg.c_str());

    if (msg == "U") {
        Serial.println("MQTT: Received 'U' on " + String(topic) +
                       ", sending 'U' to not all mesh "
                       "nodes.");
        for (const auto &pair : nodes) {
            uint32_t nodeId = pair.first;
            String nodeType = pair.second;
            if ((nodeType == "relay" && topic == "/switch/cmd") ||
                (nodeType == "switch" && topic == "/relay/cmd")) {
                Serial.println("Skipping node " + String(nodeId) + " of type " +
                               nodeType + " for topic " + String(topic));
                continue;
            }

            Serial.println("MQTT: Sending 'U' to node " + String(nodeId));
            mesh.sendSingle(nodeId, "U");
        }
    } else {
        Serial.println("MQTT: Processing non-'U' message on topic " +
                       String(topic) + " with msg: " + msg);
        size_t lastSlash = String(topic).lastIndexOf('/');
        if (lastSlash != -1 && lastSlash < strlen(topic) - 1) {
            String idStr = String(topic).substring(lastSlash + 1);
            uint32_t nodeId = idStr.toInt();
            Serial.println("MQTT: Extracted nodeId " + String(nodeId) +
                           " from topic");
            if (nodes.count(nodeId)) {
                Serial.println("MQTT: Sending msg '" + msg + "' to node " +
                               String(nodeId));
                mesh.sendSingle(nodeId, msg);
            } else {
                Serial.println("MQTT: Node " + String(nodeId) +
                               " not found in nodes map, skipping send");
            }
        } else {
            Serial.println("MQTT: Invalid topic format for nodeId extraction");
        }
    }
}

void droppedConnectionCallback(uint32_t nodeId) {
    nodes.erase(nodeId);
    Serial.printf("Node %u disconnected, total nodes: %u\n", nodeId,
                  mesh.getNodeList().size());
}

void newConnectionCallback(uint32_t nodeId) {
    uint32_t rootId = mesh.getNodeId();

    mesh.sendSingle(nodeId, String(rootId));
}

void receivedCallback(const uint32_t &from, const String &msg) {
    Serial.printf("bridge: Received from %u msg=%s\n", from, msg.c_str());

    if (msg == "R") {
        Serial.println("New node type: relay");
        nodes[from] = "relay";
    } else if (msg == "S") {
        Serial.println("New node type: switch");
        nodes[from] = "switch";
    } else if (msg[0] >= 97 && msg[0] < 97 + NLIGHTS && msg.length() == 2) {
        if (WiFi.status() != WL_CONNECTED) return;
        if (!mqttClient.connected()) mqttConnect();

        String topic = "/switch/" + String(from);

        Serial.println("Publishing to topic: " + topic);
        mqttClient.publish(topic.c_str(), msg.c_str());
    } else if (msg[0] >= 65 && msg[0] < 65 + NLIGHTS && msg.length() == 2) {
        Serial.println("0");
        if (WiFi.status() != WL_CONNECTED) return;
        if (!mqttClient.connected()) mqttConnect();

        Serial.println("1");
        String topic = "/relay/state/" + String(from);

        Serial.println("Publishing to topic: " + topic);
        mqttClient.publish(topic.c_str(), msg.c_str());
    }
}

void setup() {
    Serial.begin(115200);

    pixels.begin();
    pixels.setBrightness(5);
    setLedColor(0, 0, 0);

    meshInit();
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

    if (millis() - lastPrint >= 10000) {
        lastPrint = millis();
        Serial.println("Connected nodes:");
        for (const auto &pair : nodes) {
            Serial.printf("Node %u: %s\n", pair.first, pair.second.c_str());
        }
    }
}