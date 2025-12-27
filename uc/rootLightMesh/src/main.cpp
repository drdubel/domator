#include <Arduino.h>
#include <ArduinoJson.h>
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
#include <utility>
#include <vector>

#include "esp_task_wdt.h"

#define HOSTNAME "mesh_root"
#define NLIGHTS 8

// Timing constants
#define NODE_STATUS_REPORT_INTERVAL 15000
#define MQTT_RECONNECT_INTERVAL 30000
#define NODE_PRINT_INTERVAL 10000
#define WIFI_CONNECT_TIMEOUT 20000
#define MQTT_CONNECT_TIMEOUT 5

// Telnet server on port 23
#define MAX_TELNET_CLIENTS 3
WiFiServer telnetServer(23);
WiFiClient telnetClients[MAX_TELNET_CLIENTS];

const int mqtt_port = 1883;
uint32_t device_id;

std::map<uint32_t, String[6]> nodesStatus;
std::map<uint32_t, String> nodes;
std::map<String, std::map<char, std::vector<std::pair<String, String>>>>
    connections;

// Function declarations
void receivedCallback(const uint32_t& from, const String& msg);
void droppedConnectionCallback(uint32_t nodeId);
void newConnectionCallback(uint32_t nodeId);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttConnect();
void meshInit();
void performFirmwareUpdate();
IPAddress getlocalIP();
void telnetPrint(const String& msg);
void telnetPrintln(const String& msg);

IPAddress myIP(0, 0, 0, 0);
painlessMesh mesh;
WiFiClient wifiClient;
PubSubClient mqttClient(mqtt_broker, mqtt_port, wifiClient);

// Timing variables
static unsigned long lastPrint = 0;
static unsigned long lastMqttReconnect = 0;
static unsigned long lastNodeStatusReport = 0;

// Task handles
TaskHandle_t telnetTaskHandle = NULL;
TaskHandle_t wifiMqttTaskHandle = NULL;
TaskHandle_t statusTaskHandle = NULL;
TaskHandle_t nodeStatusTaskHandle = NULL;
TaskHandle_t meshCheckTaskHandle = NULL;

// Node-parent mapping
std::map<uint32_t, uint32_t> nodeParentMap;

// OTA update flag
volatile bool otaInProgress = false;

String fw_md5;  // MD5 of the firmware as flashed

// Telnet helper functions
void telnetPrint(const String& msg) {
    for (int i = 0; i < MAX_TELNET_CLIENTS; i++) {
        if (telnetClients[i] && telnetClients[i].connected()) {
            telnetClients[i].print(msg);
        }
    }
}

void telnetPrintln(const String& msg) { telnetPrint(msg + "\n"); }

// Override Serial.print functions to also send to Telnet
#define DEBUG_PRINT(x)          \
    do {                        \
        Serial.print(x);        \
        telnetPrint(String(x)); \
    } while (0)
#define DEBUG_PRINTLN(x)          \
    do {                          \
        Serial.println(x);        \
        telnetPrintln(String(x)); \
    } while (0)
#define DEBUG_PRINTF(fmt, ...)                          \
    do {                                                \
        char buf[1024];                                 \
        snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); \
        Serial.print(buf);                              \
        telnetPrint(String(buf));                       \
    } while (0)

void handleTelnet(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!WiFi.isConnected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Check for new clients
        if (telnetServer.hasClient()) {
            bool clientConnected = false;

            // Find free slot
            for (int i = 0; i < MAX_TELNET_CLIENTS; i++) {
                if (!telnetClients[i] || !telnetClients[i].connected()) {
                    if (telnetClients[i]) {
                        telnetClients[i].stop();
                    }
                    telnetClients[i] = telnetServer.available();
                    telnetClients[i].println(
                        "\n=== ESP32 Mesh Root - Telnet Monitor ===");
                    telnetClients[i].printf("Device ID: %u\n", device_id);
                    telnetClients[i].printf("IP: %s\n",
                                            WiFi.localIP().toString().c_str());
                    telnetClients[i].println(
                        "=====================================\n");
                    clientConnected = true;
                    DEBUG_PRINTF("Telnet: Client %d connected\n", i);
                    break;
                }
            }

            if (!clientConnected) {
                WiFiClient rejectClient = telnetServer.available();
                rejectClient.println(
                    "Max clients reached. Connection rejected.");
                rejectClient.stop();
            }
        }

        // Handle client input
        for (int i = 0; i < MAX_TELNET_CLIENTS; i++) {
            if (telnetClients[i] && telnetClients[i].connected()) {
                while (telnetClients[i].available()) {
                    char c = telnetClients[i].read();
                    // Echo back to client
                    telnetClients[i].write(c);
                    // Also write to Serial
                    Serial.write(c);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));  // Small delay to avoid busy loop
    }
}

void otaTask(void* pv) {
    otaInProgress = true;

    esp_task_wdt_deinit();  // REQUIRED

    mqttClient.disconnect();

    DEBUG_PRINTLN("[OTA] Stopping mesh...");
    mesh.stop();

    vTaskDelay(pdMS_TO_TICKS(1000));

    performFirmwareUpdate();  // now safe
}

void performFirmwareUpdate() {
    DEBUG_PRINTLN("[OTA] Starting firmware update...");

    delay(1000);  // Give time for cleanup

    DEBUG_PRINTLN("[OTA] Switching to STA mode...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    DEBUG_PRINT("[OTA] Connecting to WiFi");
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startTime > WIFI_CONNECT_TIMEOUT) {
            DEBUG_PRINTLN("\n[OTA] WiFi connection timeout, restarting...");
            ESP.restart();
            return;
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
        DEBUG_PRINT(".");
    }
    DEBUG_PRINTLN(" connected!");
    DEBUG_PRINTF("[OTA] IP: %s\n", WiFi.localIP().toString().c_str());

    WiFiClientSecure* client = new WiFiClientSecure();
    if (!client) {
        DEBUG_PRINTLN("[OTA] Failed to allocate WiFiClientSecure");
        ESP.restart();
        return;
    }

    client->setInsecure();
    HTTPClient http;
    http.setTimeout(30000);

    DEBUG_PRINTLN("[OTA] Connecting to update server...");
    if (!http.begin(*client, firmware_url)) {
        DEBUG_PRINTLN("[OTA] Failed to begin HTTP connection");
        delete client;
        ESP.restart();
        return;
    }

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int contentLength = http.getSize();
        DEBUG_PRINTF("[OTA] Firmware size: %d bytes\n", contentLength);

        if (contentLength <= 0) {
            DEBUG_PRINTLN("[OTA] Invalid content length");
            http.end();
            delete client;
            ESP.restart();
            return;
        }

        if (!Update.begin(contentLength)) {
            DEBUG_PRINTF(
                "[OTA] Not enough space. Required: %d, Available: %d\n",
                contentLength, ESP.getFreeSketchSpace());
            http.end();
            delete client;
            ESP.restart();
            return;
        }

        DEBUG_PRINTLN("[OTA] Writing firmware...");
        WiFiClient* stream = http.getStreamPtr();
        size_t written = Update.writeStream(*stream);
        if (written != contentLength) {
            DEBUG_PRINTF(
                "[OTA] Written bytes mismatch! Expected: %d, got: %d\n",
                contentLength, written);
            Update.abort();
            http.end();
            delete client;
            ESP.restart();
            return;
        }

        DEBUG_PRINTF("[OTA] Written %d/%d bytes\n", (int)written,
                     contentLength);

        if (Update.end()) {
            if (Update.isFinished()) {
                DEBUG_PRINTLN("[OTA] Update finished successfully!");
                http.end();
                delete client;
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                ESP.restart();
            } else {
                DEBUG_PRINTLN("[OTA] Update not finished properly");
                Update.printError(Serial);
            }
        } else {
            DEBUG_PRINTF("[OTA] Update error: %d\n", Update.getError());
            Update.printError(Serial);
        }
    } else {
        DEBUG_PRINTF("[OTA] HTTP GET failed, code: %d\n", httpCode);
    }

    http.end();
    delete client;

    DEBUG_PRINTLN("[OTA] Update failed, restarting...");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP.restart();
}

IPAddress getlocalIP() { return IPAddress(mesh.getStationIP()); }

bool isValidJson(const String& s) {
    JsonDocument doc;  // Small doc is enough for validation
    DeserializationError err = deserializeJson(doc, s);
    return !err;
}

void parseConnections(JsonObject root) {
    if (root.isNull()) return;

    for (JsonPair idPair : root) {
        String id = idPair.key().c_str();
        JsonObject letterObj = idPair.value().as<JsonObject>();

        for (JsonPair letterPair : letterObj) {
            char letter = letterPair.key().c_str()[0];
            JsonArray arr = letterPair.value().as<JsonArray>();

            std::vector<std::pair<String, String>> vec;
            vec.reserve(arr.size());

            for (JsonArray item : arr) {
                if (item.size() >= 2) {
                    String first = item[0].as<const char*>();
                    String second = item[1].as<const char*>();
                    vec.emplace_back(first, second);
                }
            }

            connections[id][letter] = std::move(vec);
        }
    }

    // Debug print all parsed connections
    DEBUG_PRINTLN("Connections dump:");
    for (const auto& idEntry : connections) {
        DEBUG_PRINTF("Node %s:\n", idEntry.first.c_str());
        for (const auto& letterEntry : idEntry.second) {
            DEBUG_PRINTF("  %c =>", letterEntry.first);
            if (letterEntry.second.empty()) {
                DEBUG_PRINTLN(" (none)");
                continue;
            }
            for (size_t i = 0; i < letterEntry.second.size(); ++i) {
                const auto& p = letterEntry.second[i];
                DEBUG_PRINTF(" [%s,%s]", p.first.c_str(), p.second.c_str());
            }
            DEBUG_PRINTLN("");
        }
    }
    DEBUG_PRINTF("Total connection roots: %d\n", (int)connections.size());
}

void mqttConnect() {
    if (WiFi.status() != WL_CONNECTED) {
        DEBUG_PRINTLN("MQTT: WiFi not connected, skipping MQTT connection");
        return;
    }

    DEBUG_PRINTF("MQTT: Connecting to broker at %s:%d as %u\n",
                 mqtt_broker.toString().c_str(), mqtt_port, device_id);

    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(90);
    mqttClient.setSocketTimeout(30);

    int retries = 0;
    while (!mqttClient.connected() && retries < MQTT_CONNECT_TIMEOUT) {
        String clientId = String(device_id);

        if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
            DEBUG_PRINTLN("MQTT: Connected successfully");

            mqttClient.subscribe("/switch/cmd/+");
            mqttClient.subscribe("/switch/cmd");
            mqttClient.subscribe("/relay/cmd/+");
            mqttClient.subscribe("/relay/cmd");

            mqttClient.publish("/switch/state/root", "connected", true);

            DEBUG_PRINTF("MQTT: Free heap after connect: %d bytes\n",
                         ESP.getFreeHeap());
            return;
        } else {
            DEBUG_PRINTF("MQTT: Connection failed, rc=%d, retry %d/%d\n",
                         mqttClient.state(), retries + 1, MQTT_CONNECT_TIMEOUT);
            retries++;
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }

    if (!mqttClient.connected()) {
        DEBUG_PRINTLN("MQTT: Failed to connect after retries");
    }
}

void meshInit() {
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);

    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA);
    mesh.stationManual(WIFI_SSID, WIFI_PASSWORD);

    mesh.setRoot(true);
    mesh.setContainsRoot(true);
    mesh.setHostname(HOSTNAME);

    mesh.onReceive(&receivedCallback);
    mesh.onDroppedConnection(&droppedConnectionCallback);
    mesh.onNewConnection(&newConnectionCallback);

    device_id = mesh.getNodeId();
    DEBUG_PRINTF("ROOT: Device ID: %u\n", device_id);
    DEBUG_PRINTF("ROOT: Free heap: %d bytes\n", ESP.getFreeHeap());
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg;
    msg.reserve(length + 1);
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    if (msg == "P") {
        return;
    }

    DEBUG_PRINTF("MQTT: [%s] %s\n", topic, msg.c_str());

    if (strcmp(topic, "/switch/cmd/root") == 0) {
        if (msg == "U") {
            DEBUG_PRINTLN("MQTT: Firmware update requested for root node");

            otaInProgress = true;

            return;
        }

        if (isValidJson(msg)) {
            DEBUG_PRINTLN("MQTT: Updating connections from JSON");
            JsonDocument doc;
            deserializeJson(doc, msg);

            parseConnections(doc.as<JsonObject>());
            DEBUG_PRINTLN("MQTT: Connections updated from JSON");

            return;
        }
    }

    if (msg == "U") {
        DEBUG_PRINTLN("MQTT: Broadcasting firmware update to mesh nodes");

        for (const auto& pair : nodes) {
            uint32_t nodeId = pair.first;
            String nodeType = pair.second;

            if ((nodeType == "relay" && strcmp(topic, "/switch/cmd") == 0) ||
                (nodeType == "switch" && strcmp(topic, "/relay/cmd") == 0)) {
                DEBUG_PRINTF("MQTT: Skipping node %u (type: %s) for topic %s\n",
                             nodeId, nodeType.c_str(), topic);
                continue;
            }

            DEBUG_PRINTF("MQTT: Sending update command to node %u\n", nodeId);
            mesh.sendSingle(nodeId, "U");
        }
        return;
    }

    size_t lastSlash = String(topic).lastIndexOf('/');
    if (lastSlash != -1 && lastSlash < strlen(topic) - 1) {
        String idStr = String(topic).substring(lastSlash + 1);
        uint32_t nodeId = idStr.toInt();

        if (nodeId == 0 && idStr != "0") {
            DEBUG_PRINTF("MQTT: Invalid node ID in topic: %s\n", topic);
            return;
        }

        if (nodes.count(nodeId)) {
            DEBUG_PRINTF("MQTT: Forwarding to node %u: %s\n", nodeId,
                         msg.c_str());
            mesh.sendSingle(nodeId, msg);
        } else {
            DEBUG_PRINTF("MQTT: Node %u not found in mesh\n", nodeId);
        }
    } else {
        DEBUG_PRINTF("MQTT: Cannot extract node ID from topic: %s\n", topic);
    }
}

void droppedConnectionCallback(uint32_t nodeId) {
    if (nodes.erase(nodeId)) {
        DEBUG_PRINTF("MESH: Node %u disconnected (removed from registry)\n",
                     nodeId);
    }
    DEBUG_PRINTF("MESH: Total nodes: %u\n", mesh.getNodeList().size());
    DEBUG_PRINTF("MESH: Free heap after disconnect: %d bytes\n",
                 ESP.getFreeHeap());
}

void newConnectionCallback(uint32_t nodeId) {
    DEBUG_PRINTF("MESH: New connection from node %u\n", nodeId);
    DEBUG_PRINTF("MESH: Total nodes: %u\n", mesh.getNodeList().size());
    DEBUG_PRINTF("MESH: Free heap after new connection: %d bytes\n",
                 ESP.getFreeHeap());

    mesh.sendSingle(nodeId, "Q");
}

void handleSwitchMessage(const uint32_t& from, const char output,
                         int state = -1) {
    DEBUG_PRINTF("MESH: Switch message from %u: %c\n", from, output);

    std::vector<std::pair<String, String>> targets =
        connections[String(from)][output];
    DEBUG_PRINTF("MESH: Found %zu targets for switch %u and message %c\n",
                 targets.size(), from, output);

    for (const auto& target : targets) {
        DEBUG_PRINTF("MESH: Processing target %s with command %s\n",
                     target.first.c_str(), target.second.c_str());
        String relayIdStr = target.first;
        String command = target.second;

        uint32_t relayId = relayIdStr.toInt();
        if (state == -1) {
            mesh.sendSingle(relayId, command);
            DEBUG_PRINTF("MESH: Sent to relay %s command %s\n",
                         relayIdStr.c_str(), command.c_str());
        } else {
            command += String(state);
            mesh.sendSingle(relayId, command);
            DEBUG_PRINTF("MESH: Sent to relay %s command %s\n",
                         relayIdStr.c_str(), command.c_str());
        }
    }
}

void handleRelayMessage(const uint32_t& from, const String& msg) {
    DEBUG_PRINTF("MESH: Relay message from %u: %s\n", from, msg.c_str());

    if (WiFi.status() != WL_CONNECTED) {
        DEBUG_PRINTLN("MESH: WiFi not connected, cannot publish to MQTT");
        return;
    }

    if (!mqttClient.connected()) {
        DEBUG_PRINTLN("MESH: MQTT not connected, cannot publish");
        return;
    }

    String topic = "/relay/state/" + String(from);
    if (mqttClient.publish(topic.c_str(), msg.c_str())) {
        DEBUG_PRINTF("MQTT: Published [%s] %s\n", topic.c_str(), msg.c_str());
    } else {
        DEBUG_PRINTF("MQTT: Failed to publish [%s] %s\n", topic.c_str(),
                     msg.c_str());
    }
}

void createNode(uint32_t nodeId, String type) {
    nodes[nodeId] = type;
    nodesStatus[nodeId][0] = "-1";
    nodesStatus[nodeId][1] = "-1";
    nodesStatus[nodeId][2] = "-1";
    nodesStatus[nodeId][3] = "-1";
    nodesStatus[nodeId][4] = "-1";
    nodesStatus[nodeId][5] = String(millis());
}

void receivedCallback(const uint32_t& from, const String& msg) {
    DEBUG_PRINTF("MESH: [%u] %s\n", from, msg.c_str());

    if (msg == "R") {
        createNode(from, "relay");
        DEBUG_PRINTF("MESH: Registered node %u as relay\n", from);
        mesh.sendSingle(from, "A");
        return;
    }

    if (msg == "S") {
        createNode(from, "switch");
        DEBUG_PRINTF("MESH: Registered node %u as switch\n", from);
        mesh.sendSingle(from, "A");
        return;
    }

    if (msg[0] >= 'a' && msg[0] < 'a' + NLIGHTS) {
        if (msg.length() == 1) {
            handleSwitchMessage(from, msg[0]);
        } else {
            handleSwitchMessage(from, msg[0], msg[1] - '0');
        }
        vTaskDelay(5 / portTICK_PERIOD_MS);
        return;
    }

    if (msg.length() == 2 && msg[0] >= 'A' && msg[0] < 'A' + NLIGHTS) {
        handleRelayMessage(from, msg);
        vTaskDelay(5 / portTICK_PERIOD_MS);
        return;
    }

    if (msg == "P") {
        DEBUG_PRINTLN("MESH: Received ping, sending pong");
        mqttClient.publish(("/relay/state/" + String(from)).c_str(), "P");
        return;
    }

    if (isValidJson(msg)) {
        DEBUG_PRINTLN("MESH: Received JSON message, updating node status");
        JsonDocument doc;
        deserializeJson(doc, msg);

        JsonArray obj = doc.as<JsonArray>();
        for (int i = 0; i < 6; i++) {
            nodesStatus[from][i] = obj[i].as<String>();
        }
        nodesStatus[from][5] = String(millis());

        DEBUG_PRINTF("MESH: Updated status for node %u\n", from);

        return;
    }

    DEBUG_PRINTF("MESH: Unknown message format from %u: %s\n", from,
                 msg.c_str());
}

void checkWiFi() {
    IPAddress currentIP = getlocalIP();
    if (myIP != currentIP && currentIP != IPAddress(0, 0, 0, 0)) {
        myIP = currentIP;
        DEBUG_PRINTLN("WiFi: Connected to external network");
        DEBUG_PRINTF("WiFi: IP address: %s\n", myIP.toString().c_str());

        // Start Telnet server
        if (!telnetServer) {
            DEBUG_PRINTLN("Telnet: Starting server...");

            telnetServer.begin();
            telnetServer.setNoDelay(true);
            DEBUG_PRINTLN("Telnet: Server started on port 23");
            DEBUG_PRINTF("Telnet: Connect using: telnet %s\n",
                         myIP.toString().c_str());
        }
    }
}

void checkWiFiAndMQTT(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        checkWiFi();

        while (!WiFi.isConnected()) {
            DEBUG_PRINTLN("WiFi: Not connected, attempting reconnection...");
            vTaskDelay(5000 / portTICK_PERIOD_MS);
        }

        if (!mqttClient.connected()) {
            unsigned long now = millis();
            if (now - lastMqttReconnect > MQTT_RECONNECT_INTERVAL) {
                lastMqttReconnect = now;
                DEBUG_PRINTLN("MQTT: Attempting reconnection...");
                mqttConnect();
            }
        } else {
            mqttClient.loop();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void checkMesh(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        for (auto nodeId : mesh.getNodeList()) {
            if (nodes.find(nodeId) == nodes.end()) {
                DEBUG_PRINTF(
                    "MESH: Detected new node %u, requesting registration\n",
                    nodeId);
                mesh.sendSingle(nodeId, "Q");
                vTaskDelay(25 / portTICK_PERIOD_MS);
            }
        }

        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

void buildParentMap(const painlessmesh::protocol::NodeTree& node,
                    uint32_t parent = 0) {
    nodeParentMap[node.nodeId] = parent;
    for (const auto& child : node.subs) {
        buildParentMap(child, node.nodeId);
    }
}

void sendNodeStatusReport(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        DEBUG_PRINTF("MESH: Preparing node status report...\n");

        lastNodeStatusReport = millis();
        while (WiFi.status() != WL_CONNECTED || !mqttClient.connected()) {
            DEBUG_PRINTLN(
                "MESH: WiFi or MQTT not connected, cannot send node status "
                "report");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }

        JsonDocument doc;
        JsonObject nodesDict = doc.to<JsonObject>();

        auto meshNodes = mesh.getNodeList();
        painlessmesh::protocol::NodeTree tree = mesh.asNodeTree();
        buildParentMap(tree);

        for (auto nodeId : meshNodes) {
            JsonObject status = nodesDict[String(nodeId)].to<JsonObject>();

            status["rssi"] = nodesStatus[nodeId][0];
            status["uptime"] = nodesStatus[nodeId][1];
            status["clicks"] = nodesStatus[nodeId][2];
            status["firmware"] = nodesStatus[nodeId][3];
            status["disconnects"] = nodesStatus[nodeId][4];
            status["last_seen"] =
                String((millis() - nodesStatus[nodeId][5].toInt()) / 1000);
            status["type"] = nodes[nodeId];
            status["status"] =
                status["last_seen"].as<uint32_t>() < 60 ? "online" : "offline";
            status["parent"] = String(nodeParentMap[nodeId]);

            if (status["status"] == "offline") {
                nodes.erase(nodeId);
            }
        }

        JsonObject rootNode =
            nodesDict[String(mesh.getNodeId())].to<JsonObject>();
        rootNode["last_seen"] = "0";
        rootNode["rssi"] = String(WiFi.RSSI());
        rootNode["uptime"] = String(millis() / 1000);
        rootNode["clicks"] = "0";
        rootNode["firmware"] = fw_md5;
        rootNode["disconnects"] = "0";
        rootNode["type"] = "root";
        rootNode["status"] = "online";
        rootNode["parent"] = "root";
        rootNode["free_heap"] = String(ESP.getFreeHeap());

        String message;
        serializeJson(doc, message);
        DEBUG_PRINTF("MESH: Node status report JSON: %s\n", message.c_str());

        if (mqttClient.publish("/switch/state/root", message.c_str())) {
            DEBUG_PRINTLN("MESH: Node status report sent successfully");
        } else {
            DEBUG_PRINTLN("MESH: Failed to send node status report");
        }

        vTaskDelay(pdMS_TO_TICKS(NODE_STATUS_REPORT_INTERVAL));
    }
}

void meshStatusReport() {
    DEBUG_PRINTLN("\nRegistered Nodes:");
    if (nodes.empty()) {
        DEBUG_PRINTLN("  (none)");
    } else {
        for (const auto& pair : nodes) {
            DEBUG_PRINTF("  Node %u: %s\n", pair.first, pair.second.c_str());
        }
    }

    auto meshNodes = mesh.getNodeList();
    DEBUG_PRINTF("\nMesh Network: %u node(s)\n", meshNodes.size());
    for (auto node : meshNodes) {
        DEBUG_PRINTF("  %u\n", node);
    }

    painlessmesh::protocol::NodeTree tree = mesh.asNodeTree();
    DEBUG_PRINTLN("MESH: Current Node Tree:");
    DEBUG_PRINTF("%s\n", tree.toString().c_str());  // <-- Use toString()
}

void statusReport(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        lastPrint = millis();

        DEBUG_PRINTLN("\n--- Status Report ---");
        DEBUG_PRINTF("Firmware MD5: %s\n", fw_md5.c_str());
        DEBUG_PRINTF("WiFi: %s\n", WiFi.status() == WL_CONNECTED
                                       ? "Connected"
                                       : "Disconnected");
        DEBUG_PRINTF("MQTT: %s\n",
                     mqttClient.connected() ? "Connected" : "Disconnected");
        DEBUG_PRINTF("Free Heap: %d bytes\n", ESP.getFreeHeap());
        DEBUG_PRINTF("Uptime: %lu seconds\n", millis() / 1000);

        meshStatusReport();
        DEBUG_PRINTLN("-------------------\n");

        vTaskDelay(pdMS_TO_TICKS(20000));
    }
}

void setup() {
    Serial.begin(115200);
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    fw_md5 = ESP.getSketchMD5();  // MD5 of the firmware as flashed

    DEBUG_PRINTLN("\n\n========================================");
    DEBUG_PRINTLN("ESP32 Mesh Root Node Starting...");
    DEBUG_PRINTF("Chip Model: %s\n", ESP.getChipModel());
    DEBUG_PRINTF("Chip Revision: %d\n", ESP.getChipRevision());
    DEBUG_PRINTF("CPU Frequency: %d MHz\n", ESP.getCpuFreqMHz());
    DEBUG_PRINTF("Free Heap: %d bytes\n", ESP.getFreeHeap());
    DEBUG_PRINTF("Flash Size: %d bytes\n", ESP.getFlashChipSize());
    DEBUG_PRINTF("Firmware MD5: %s\n", fw_md5.c_str());
    DEBUG_PRINTLN("========================================\n");

    meshInit();

    // Start Telnet handler task
    xTaskCreatePinnedToCore(handleTelnet, "TelnetTask", 16384, NULL, 1,
                            &telnetTaskHandle, 1);

    // Check WiFi and MQTT
    xTaskCreatePinnedToCore(checkWiFiAndMQTT, "WiFiMQTTTask", 16384, NULL, 2,
                            &wifiMqttTaskHandle, 1);

    // Periodic status report
    xTaskCreatePinnedToCore(statusReport, "StatusReportTask", 16384, NULL, 1,
                            &statusTaskHandle, 1);

    // Periodic node status report
    xTaskCreatePinnedToCore(sendNodeStatusReport, "NodeStatusReportTask", 16384,
                            NULL, 1, &nodeStatusTaskHandle, 1);

    // Mesh checker task
    xTaskCreatePinnedToCore(checkMesh, "MeshCheckTask", 16384, NULL, 1,
                            &meshCheckTaskHandle, 1);
}

void loop() {
    static bool otaTaskStarted = false;

    if (otaInProgress && !otaTaskStarted) {
        otaTaskStarted = true;

        xTaskCreatePinnedToCore(otaTask, "OTA", 16384, NULL, 5, NULL,
                                0  // Core 0
        );
    }

    if (!otaInProgress) {
        mesh.update();
    } else {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}