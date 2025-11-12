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

// Timing constants
#define MQTT_RECONNECT_INTERVAL 30000
#define NODE_PRINT_INTERVAL 10000
#define WIFI_CONNECT_TIMEOUT 20000
#define MQTT_CONNECT_TIMEOUT 5

IPAddress mqtt_broker(192, 168, 3, 10);
const int mqtt_port = 1883;
const char* mqttUser = "mesh_root";
uint32_t device_id;

std::map<uint32_t, String> nodes;

// Function declarations
void receivedCallback(const uint32_t& from, const String& msg);
void droppedConnectionCallback(uint32_t nodeId);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttConnect();
void meshInit();
void performFirmwareUpdate();
IPAddress getlocalIP();

IPAddress myIP(0, 0, 0, 0);
painlessMesh mesh;
WiFiClient wifiClient;
PubSubClient mqttClient(mqtt_broker, mqtt_port, wifiClient);

// Timing variables
static unsigned long lastPrint = 0;
static unsigned long lastMqttReconnect = 0;

const char* firmware_url =
    "https://czupel.dry.pl/static/data/root/firmware.bin";

void performFirmwareUpdate() {
    Serial.println("[OTA] Starting firmware update...");

    // Clean up existing connections
    if (mqttClient.connected()) {
        mqttClient.disconnect();
    }

    Serial.println("[OTA] Stopping mesh...");
    mesh.stop();

    delay(1000);  // Give time for cleanup

    Serial.println("[OTA] Switching to STA mode...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("[OTA] Connecting to WiFi");
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startTime > WIFI_CONNECT_TIMEOUT) {
            Serial.println("\n[OTA] WiFi connection timeout, restarting...");
            ESP.restart();
            return;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println(" connected!");
    Serial.printf("[OTA] IP: %s\n", WiFi.localIP().toString().c_str());

    WiFiClientSecure* client = new WiFiClientSecure();
    if (!client) {
        Serial.println("[OTA] Failed to allocate WiFiClientSecure");
        ESP.restart();
        return;
    }

    client->setInsecure();
    HTTPClient http;
    http.setTimeout(30000);  // 30 second timeout

    Serial.println("[OTA] Connecting to update server...");
    if (!http.begin(*client, firmware_url)) {
        Serial.println("[OTA] Failed to begin HTTP connection");
        delete client;
        ESP.restart();
        return;
    }

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int contentLength = http.getSize();
        Serial.printf("[OTA] Firmware size: %d bytes\n", contentLength);

        if (contentLength <= 0) {
            Serial.println("[OTA] Invalid content length");
            http.end();
            delete client;
            ESP.restart();
            return;
        }

        if (!Update.begin(contentLength)) {
            Serial.printf(
                "[OTA] Not enough space. Required: %d, Available: %d\n",
                contentLength, ESP.getFreeSketchSpace());
            http.end();
            delete client;
            ESP.restart();
            return;
        }

        Serial.println("[OTA] Writing firmware...");
        WiFiClient* stream = http.getStreamPtr();
        size_t written = Update.writeStream(*stream);

        Serial.printf("[OTA] Written %d/%d bytes\n", (int)written,
                      contentLength);

        if (Update.end()) {
            if (Update.isFinished()) {
                Serial.println("[OTA] Update finished successfully!");
                http.end();
                delete client;
                delay(1000);
                ESP.restart();
            } else {
                Serial.println("[OTA] Update not finished properly");
                Update.printError(Serial);
            }
        } else {
            Serial.printf("[OTA] Update error: %d\n", Update.getError());
            Update.printError(Serial);
        }
    } else {
        Serial.printf("[OTA] HTTP GET failed, code: %d\n", httpCode);
    }

    http.end();
    delete client;

    Serial.println("[OTA] Update failed, restarting...");
    delay(1000);
    ESP.restart();
}

IPAddress getlocalIP() { return IPAddress(mesh.getStationIP()); }

void mqttConnect() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("MQTT: WiFi not connected, skipping MQTT connection");
        return;
    }

    Serial.printf("MQTT: Connecting to broker at %s:%d as %u\n",
                  mqtt_broker.toString().c_str(), mqtt_port, device_id);

    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(90);
    mqttClient.setSocketTimeout(30);

    int retries = 0;
    while (!mqttClient.connected() && retries < MQTT_CONNECT_TIMEOUT) {
        String clientId = String(device_id);

        if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
            Serial.println("MQTT: Connected successfully");

            // Subscribe to topics
            mqttClient.subscribe("/switch/cmd/+");
            mqttClient.subscribe("/switch/cmd");
            mqttClient.subscribe("/relay/cmd/+");
            mqttClient.subscribe("/relay/cmd");

            // Publish connection state
            mqttClient.publish("/switch/state/root", "connected", true);

            Serial.printf("MQTT: Free heap after connect: %d bytes\n",
                          ESP.getFreeHeap());
            return;
        } else {
            Serial.printf("MQTT: Connection failed, rc=%d, retry %d/%d\n",
                          mqttClient.state(), retries + 1,
                          MQTT_CONNECT_TIMEOUT);
            retries++;
            delay(1000);
        }
    }

    if (!mqttClient.connected()) {
        Serial.println("MQTT: Failed to connect after retries");
    }
}

void meshInit() {
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);

    // Initialize with longer timeout and more retries
    // Parameters: (prefix, password, port, connectivity, channel, hidden,
    // maxConnections)
    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA, 6, 0, 10);
    mesh.stationManual(WIFI_SSID, WIFI_PASSWORD);

    mesh.setRoot(true);
    mesh.setContainsRoot(true);
    mesh.setHostname(HOSTNAME);

    mesh.onReceive(&receivedCallback);
    mesh.onDroppedConnection(&droppedConnectionCallback);

    device_id = mesh.getNodeId();
    Serial.printf("ROOT: Device ID: %u\n", device_id);
    Serial.printf("ROOT: Free heap: %d bytes\n", ESP.getFreeHeap());
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    // Build message string
    String msg;
    msg.reserve(length + 1);
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    Serial.printf("MQTT: [%s] %s\n", topic, msg.c_str());

    // Handle firmware update command
    if (msg == "U") {
        if (strcmp(topic, "/switch/cmd/root") == 0) {
            Serial.println("MQTT: Firmware update requested for root node");
            performFirmwareUpdate();
            return;
        }

        // Broadcast update to mesh nodes
        Serial.println("MQTT: Broadcasting firmware update to mesh nodes");

        for (const auto& pair : nodes) {
            uint32_t nodeId = pair.first;
            String nodeType = pair.second;

            // Skip nodes that don't match the topic type
            if ((nodeType == "relay" && strcmp(topic, "/switch/cmd") == 0) ||
                (nodeType == "switch" && strcmp(topic, "/relay/cmd") == 0)) {
                Serial.printf(
                    "MQTT: Skipping node %u (type: %s) for topic %s\n", nodeId,
                    nodeType.c_str(), topic);
                continue;
            }

            Serial.printf("MQTT: Sending update command to node %u\n", nodeId);
            mesh.sendSingle(nodeId, "U");
        }
        return;
    }

    // Handle regular commands to specific nodes
    size_t lastSlash = String(topic).lastIndexOf('/');
    if (lastSlash != -1 && lastSlash < strlen(topic) - 1) {
        String idStr = String(topic).substring(lastSlash + 1);
        uint32_t nodeId = idStr.toInt();

        if (nodeId == 0 && idStr != "0") {
            Serial.printf("MQTT: Invalid node ID in topic: %s\n", topic);
            return;
        }

        if (nodes.count(nodeId)) {
            Serial.printf("MQTT: Forwarding to node %u: %s\n", nodeId,
                          msg.c_str());
            mesh.sendSingle(nodeId, msg);
        } else {
            Serial.printf("MQTT: Node %u not found in mesh\n", nodeId);
        }
    } else {
        Serial.printf("MQTT: Cannot extract node ID from topic: %s\n", topic);
    }
}

void droppedConnectionCallback(uint32_t nodeId) {
    if (nodes.erase(nodeId)) {
        Serial.printf("MESH: Node %u disconnected (removed from registry)\n",
                      nodeId);

        // Publish disconnection to MQTT if available
        if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
            String topic = "/node/disconnect/" + String(nodeId);
            mqttClient.publish(topic.c_str(), "offline", true);
        }
    }
    Serial.printf("MESH: Total nodes: %u\n", mesh.getNodeList().size());
    Serial.printf("MESH: Free heap after disconnect: %d bytes\n",
                  ESP.getFreeHeap());
}

void receivedCallback(const uint32_t& from, const String& msg) {
    Serial.printf("MESH: [%u] %s\n", from, msg.c_str());

    // Handle node type registration
    if (msg == "R") {
        nodes[from] = "relay";
        Serial.printf("MESH: Registered node %u as relay\n", from);
        return;
    }

    if (msg == "S") {
        nodes[from] = "switch";
        Serial.printf("MESH: Registered node %u as switch\n", from);
        return;
    }

    // Handle switch state messages (lowercase letters a-g)
    if (msg.length() == 2 && msg[0] >= 'a' && msg[0] < 'a' + NLIGHTS) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("MESH: WiFi not connected, cannot publish to MQTT");
            return;
        }

        if (!mqttClient.connected()) {
            Serial.println("MESH: MQTT not connected, cannot publish");
            return;
        }

        String topic = "/switch/state/" + String(from);
        if (mqttClient.publish(topic.c_str(), msg.c_str())) {
            Serial.printf("MQTT: Published [%s] %s\n", topic.c_str(),
                          msg.c_str());
        } else {
            Serial.printf("MQTT: Failed to publish [%s] %s\n", topic.c_str(),
                          msg.c_str());
        }
        return;
    }

    // Handle relay state messages (uppercase letters A-G)
    if (msg.length() == 2 && msg[0] >= 'A' && msg[0] < 'A' + NLIGHTS) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("MESH: WiFi not connected, cannot publish to MQTT");
            return;
        }

        if (!mqttClient.connected()) {
            Serial.println("MESH: MQTT not connected, cannot publish");
            return;
        }

        String topic = "/relay/state/" + String(from);
        if (mqttClient.publish(topic.c_str(), msg.c_str())) {
            Serial.printf("MQTT: Published [%s] %s\n", topic.c_str(),
                          msg.c_str());
        } else {
            Serial.printf("MQTT: Failed to publish [%s] %s\n", topic.c_str(),
                          msg.c_str());
        }
        return;
    }

    Serial.printf("MESH: Unknown message format from %u: %s\n", from,
                  msg.c_str());
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n\n========================================");
    Serial.println("ESP32 Mesh Root Node Starting...");
    Serial.printf("Chip Model: %s\n", ESP.getChipModel());
    Serial.printf("Chip Revision: %d\n", ESP.getChipRevision());
    Serial.printf("CPU Frequency: %d MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("Flash Size: %d bytes\n", ESP.getFlashChipSize());
    Serial.println("========================================\n");

    meshInit();
}

void loop() {
    mesh.update();

    // Check for WiFi connection and get IP
    IPAddress currentIP = getlocalIP();
    if (myIP != currentIP && currentIP != IPAddress(0, 0, 0, 0)) {
        myIP = currentIP;
        Serial.println("WiFi: Connected to external network");
        Serial.printf("WiFi: IP address: %s\n", myIP.toString().c_str());

        // Attempt immediate MQTT connection
        mqttConnect();
        lastMqttReconnect = millis();
    }

    // Handle MQTT connection and loop
    if (WiFi.status() == WL_CONNECTED) {
        if (!mqttClient.connected()) {
            unsigned long now = millis();
            if (now - lastMqttReconnect > MQTT_RECONNECT_INTERVAL) {
                lastMqttReconnect = now;
                Serial.println("MQTT: Attempting reconnection...");
                mqttConnect();
            }
        } else {
            mqttClient.loop();
        }
    }

    // Periodic status report
    if (millis() - lastPrint >= NODE_PRINT_INTERVAL) {
        lastPrint = millis();

        Serial.println("\n--- Status Report ---");
        Serial.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED
                                        ? "Connected"
                                        : "Disconnected");
        Serial.printf("MQTT: %s\n",
                      mqttClient.connected() ? "Connected" : "Disconnected");
        Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
        Serial.printf("Uptime: %lu seconds\n", millis() / 1000);

        Serial.println("\nRegistered Nodes:");
        if (nodes.empty()) {
            Serial.println("  (none)");
        } else {
            for (const auto& pair : nodes) {
                Serial.printf("  Node %u: %s\n", pair.first,
                              pair.second.c_str());
            }
        }

        auto meshNodes = mesh.getNodeList();
        Serial.printf("\nMesh Network: %u node(s)\n", meshNodes.size());
        for (auto node : meshNodes) {
            Serial.printf("  %u\n", node);
        }
        Serial.println("-------------------\n");
    }
}