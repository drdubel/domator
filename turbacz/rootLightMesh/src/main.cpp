#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <credentials.h>
#include <painlessMesh.h>

#include <algorithm>
#include <map>

#define HOSTNAME "mesh_root"

#define NLIGHTS 7

IPAddress mqtt_broker(192, 168, 3, 10);
const int mqtt_port = 1883;
const char* mqttUser = "mesh_root";
uint32_t device_id;

std::map<uint32_t, String> nodes;

void receivedCallback(const uint32_t& from, const String& msg);
void droppedConnectionCallback(uint32_t nodeId);
void mqttCallback(char* topic, byte* payload, unsigned int length);

IPAddress myIP(0, 0, 0, 0);
AsyncWebServer server(80);
painlessMesh mesh;
WiFiClient wifiClient;
PubSubClient mqttClient(mqtt_broker, mqtt_port, wifiClient);

static unsigned long lastPrint = 0;

const char* firmware_url =
    "https://czupel.dry.pl/static/data/root/firmware.bin";

void performFirmwareUpdate() {
    Serial.println("[OTA] Stopping mesh...");
    mesh.stop();
    Serial.println("[OTA] Switching to STA mode...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("[OTA] Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
    }
    Serial.println(" connected!");

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    Serial.println("[OTA] Connecting to update server...");
    if (http.begin(client, firmware_url)) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            int contentLength = http.getSize();
            Serial.printf("[OTA] Firmware size: %d bytes\n", contentLength);

            if (Update.begin(contentLength)) {
                Serial.println("[OTA] Writing firmware...");
                size_t written = Update.writeStream(*http.getStreamPtr());

                Serial.printf("[OTA] Written %d/%d bytes\n", (int)written,
                              contentLength);

                if (Update.end()) {
                    if (Update.isFinished()) {
                        Serial.println("[OTA] Update finished, restarting...");
                        ESP.restart();
                    } else {
                        Serial.println(
                            "[OTA] Update not finished, something went wrong.");
                    }
                } else {
                    Serial.printf("[OTA] Update error: %d\n",
                                  Update.getError());
                }
            } else {
                Serial.println("[OTA] Not enough space for OTA.");
            }
        } else {
            Serial.printf("[OTA] HTTP GET failed, code: %d\n", httpCode);
        }
        http.end();
    } else {
        Serial.println("[OTA] Unable to connect to update server!");
    }
}

IPAddress getlocalIP() { return IPAddress(mesh.getStationIP()); }

void mqttConnect() {
    Serial.print("Connecting to MQTT broker at ");
    Serial.print(mqtt_broker);
    Serial.print(" with user ");
    Serial.println(String(device_id).c_str());
    mqttClient.setCallback(mqttCallback);

    while (!mqttClient.connected()) {
        if (mqttClient.connect(String(device_id).c_str(), MQTT_USER,
                               MQTT_PASSWORD)) {
            Serial.println("Connected to MQTT broker");
        } else {
            delay(500);
        }
    }

    mqttClient.publish("/switch/state/root", "connected");
    mqttClient.subscribe("/switch/cmd/+");
    mqttClient.subscribe("/switch/cmd");
    mqttClient.subscribe("/relay/cmd/+");
    mqttClient.subscribe("/relay/cmd");
}

void meshInit() {
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION | COMMUNICATION |
                          GENERAL);

    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA, 6, 0, 20);

    mesh.stationManual(WIFI_SSID, WIFI_PASSWORD);

    mesh.setRoot(true);
    mesh.setContainsRoot(true);
    mesh.setHostname(HOSTNAME);

    mesh.onReceive(&receivedCallback);
    mesh.onDroppedConnection(&droppedConnectionCallback);

    device_id = mesh.getNodeId();
    Serial.println("ROOT:" + String(device_id));
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg;
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    Serial.printf("MQTT: Received message on %s: %s\n", topic, msg.c_str());

    if (msg == "U") {
        if (strcmp(topic, "/switch/cmd/root") == 0) {
            Serial.println(
                "MQTT: 'U' command received for root node, "
                "performing firmware update.");
            performFirmwareUpdate();

            return;
        }

        Serial.println("MQTT: Received 'U' on " + String(topic) +
                       ", sending 'U' to some mesh "
                       "nodes.");

        for (const auto& pair : nodes) {
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

void receivedCallback(const uint32_t& from, const String& msg) {
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

        String topic = "/switch/state/" + String(from);

        Serial.println("Publishing to topic: " + topic);
        mqttClient.publish(topic.c_str(), msg.c_str());
    } else if (msg[0] >= 65 && msg[0] < 65 + NLIGHTS && msg.length() == 2) {
        if (WiFi.status() != WL_CONNECTED) return;
        if (!mqttClient.connected()) mqttConnect();

        String topic = "/relay/state/" + String(from);

        Serial.println("Publishing to topic: " + topic);
        mqttClient.publish(topic.c_str(), msg.c_str());
    }
}

void setup() {
    Serial.begin(115200);

    meshInit();
}

void loop() {
    mesh.update();

    if (myIP != getlocalIP()) {
        Serial.println("Connected to external WiFi!");
        myIP = getlocalIP();
        Serial.println("My IP is " + myIP.toString());

        mqttConnect();
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (!mqttClient.connected()) mqttConnect();

        mqttClient.loop();
    }

    if (millis() - lastPrint >= 10000) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("Not connected to WiFi, skipping node printout");
        }

        lastPrint = millis();
        Serial.println("Connected nodes:");
        for (const auto& pair : nodes) {
            Serial.printf("Node %u: %s\n", pair.first, pair.second.c_str());
        }

        auto real_nodes = mesh.getNodeList();
        for (auto node : real_nodes) {
            Serial.printf("Mesh reports node: %u\n", node);
        }
    }
}