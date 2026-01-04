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
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "esp_task_wdt.h"

#define HOSTNAME "mesh_root"
#define NLIGHTS 8

// Timing constants
#define STATUS_REPORT_INTERVAL 15000
#define MQTT_RECONNECT_INTERVAL 30000
#define WIFI_CONNECT_TIMEOUT 20000
#define MQTT_CONNECT_TIMEOUT 5

// Minimal debug - only errors and critical events
#define DEBUG_LEVEL 1  // 0=none, 1=errors only, 2=info, 3=verbose

#if DEBUG_LEVEL >= 1
#define DEBUG_ERROR(x) Serial.println(x)
#else
#define DEBUG_ERROR(x)
#endif

#if DEBUG_LEVEL >= 2
#define DEBUG_INFO(x) Serial.println(x)
#else
#define DEBUG_INFO(x)
#endif

#if DEBUG_LEVEL >= 3
#define DEBUG_VERBOSE(x) Serial.println(x)
#else
#define DEBUG_VERBOSE(x)
#endif

const int mqtt_port = 1883;
uint32_t device_id;

std::queue<std::pair<String, String>> mqttMessageQueue;
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

IPAddress myIP(0, 0, 0, 0);
painlessMesh mesh;
WiFiClient wifiClient;
PubSubClient mqttClient(mqtt_broker, mqtt_port, wifiClient);

static unsigned long lastMqttReconnect = 0;
static unsigned long lastNodeStatusReport = 0;

std::map<uint32_t, uint32_t> nodeParentMap;
volatile bool otaInProgress = false;
String fw_md5;

bool toUint64(const String& s, uint64_t& out) {
    if (s.length() == 0) return false;
    char* end;
    out = strtoull(s.c_str(), &end, 10);
    return (*end == '\0');
}

void otaTask(void* pv) {
    otaInProgress = true;
    esp_task_wdt_deinit();
    mqttClient.disconnect();
    mesh.stop();
    vTaskDelay(pdMS_TO_TICKS(1000));
    performFirmwareUpdate();
}

void performFirmwareUpdate() {
    DEBUG_INFO("[OTA] Starting update...");

    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startTime > WIFI_CONNECT_TIMEOUT) {
            DEBUG_ERROR("[OTA] WiFi timeout");
            ESP.restart();
            return;
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }

    WiFiClientSecure* client = new WiFiClientSecure();
    if (!client) {
        DEBUG_ERROR("[OTA] Client alloc failed");
        ESP.restart();
        return;
    }

    client->setInsecure();
    HTTPClient http;
    http.setTimeout(30000);

    if (!http.begin(*client, firmware_url)) {
        DEBUG_ERROR("[OTA] HTTP begin failed");
        delete client;
        ESP.restart();
        return;
    }

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int contentLength = http.getSize();

        if (contentLength <= 0 || !Update.begin(contentLength)) {
            DEBUG_ERROR("[OTA] Invalid size or begin failed");
            http.end();
            delete client;
            ESP.restart();
            return;
        }

        WiFiClient* stream = http.getStreamPtr();
        size_t written = Update.writeStream(*stream);

        if (written != contentLength) {
            DEBUG_ERROR("[OTA] Write mismatch");
            Update.abort();
            http.end();
            delete client;
            ESP.restart();
            return;
        }

        if (Update.end() && Update.isFinished()) {
            DEBUG_INFO("[OTA] Success");
            http.end();
            delete client;
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            ESP.restart();
        } else {
            DEBUG_ERROR("[OTA] Update failed");
        }
    } else {
        DEBUG_ERROR("[OTA] HTTP failed");
    }

    http.end();
    delete client;
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP.restart();
}

IPAddress getlocalIP() { return IPAddress(mesh.getStationIP()); }

bool isValidJson(const String& s) {
    JsonDocument doc;
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
                    String first;
                    if (item[0].is<const char*>()) {
                        first = item[0].as<const char*>();
                    } else if (item[0].is<long long>()) {
                        first = String(item[0].as<long long>());
                    }
                    String second = item[1].as<const char*>();
                    vec.emplace_back(first, second);
                }
            }

            connections[id][letter] = std::move(vec);
        }
    }
}

void mqttConnect() {
    if (WiFi.status() != WL_CONNECTED) return;

    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(90);
    mqttClient.setSocketTimeout(30);

    int retries = 0;
    while (!mqttClient.connected() && retries < MQTT_CONNECT_TIMEOUT) {
        String clientId = String(device_id);

        if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
            mqttClient.subscribe("/switch/cmd/+");
            mqttClient.subscribe("/switch/cmd");
            mqttClient.subscribe("/relay/cmd/+");
            mqttClient.subscribe("/relay/cmd");
            mqttClient.publish("/switch/state/root", "connected", true);
            return;
        }
        retries++;
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void meshInit() {
    mesh.setDebugMsgTypes(ERROR | STARTUP);
    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA);

    mesh.stationManual(WIFI_SSID, WIFI_PASSWORD);
    mesh.setRoot(true);
    mesh.setContainsRoot(true);
    mesh.setHostname(HOSTNAME);

    mesh.onReceive(&receivedCallback);
    mesh.onDroppedConnection(&droppedConnectionCallback);
    mesh.onNewConnection(&newConnectionCallback);

    device_id = mesh.getNodeId();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg;
    msg.reserve(length + 1);
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    if (msg == "P") return;

    if (strcmp(topic, "/switch/cmd/root") == 0) {
        if (msg == "U") {
            otaInProgress = true;
            return;
        }

        if (isValidJson(msg)) {
            JsonDocument doc;
            deserializeJson(doc, msg);

            parseConnections(doc.as<JsonObject>());

            return;
        }
    }

    if (msg == "U") {
        String path = String(topic);
        int lastSlash = path.lastIndexOf('/');
        String last = path.substring(lastSlash + 1);
        uint64_t nodeId;

        if (toUint64(last, nodeId)) {
            mesh.sendSingle(nodeId, "U");
            return;
        }

        for (const auto& pair : nodes) {
            uint32_t nodeId = pair.first;
            String nodeType = pair.second;

            if ((nodeType == "relay" && strcmp(topic, "/switch/cmd") == 0) ||
                (nodeType == "switch" && strcmp(topic, "/relay/cmd") == 0)) {
                continue;
            }
            mesh.sendSingle(nodeId, "U");
        }
        return;
    }

    size_t lastSlash = String(topic).lastIndexOf('/');
    if (lastSlash != -1 && lastSlash < strlen(topic) - 1) {
        String idStr = String(topic).substring(lastSlash + 1);
        uint32_t nodeId = idStr.toInt();

        if (nodeId != 0 || idStr == "0") {
            if (nodes.count(nodeId)) {
                mesh.sendSingle(nodeId, msg);
            }
        }
    }
}

void droppedConnectionCallback(uint32_t nodeId) { nodes.erase(nodeId); }

void newConnectionCallback(uint32_t nodeId) { mesh.sendSingle(nodeId, "Q"); }

void handleSwitchMessage(const uint32_t& from, const char output,
                         int state = -1) {
    std::vector<std::pair<String, String>> targets =
        connections[String(from)][output];

    for (const auto& target : targets) {
        String relayIdStr = target.first;
        String command = target.second;
        uint32_t relayId = relayIdStr.toInt();

        if (state != -1) {
            command += String(state);
            mesh.sendSingle(relayId, command);
        }
    }
}

void handleRelayMessage(const uint32_t& from, const String& msg) {
    if (WiFi.status() != WL_CONNECTED || !mqttClient.connected()) return;

    String topic = "/relay/state/" + String(from);
    mqttMessageQueue.push({topic, msg});
}

void receivedCallback(const uint32_t& from, const String& msg) {
    if (msg == "R") {
        nodes[from] = "relay";
        mesh.sendSingle(from, "A");
        return;
    }

    if (msg == "S") {
        nodes[from] = "switch";
        mesh.sendSingle(from, "A");
        return;
    }

    if (msg[0] >= 'a' && msg[0] < 'a' + NLIGHTS) {
        if (msg.length() == 1) {
            handleSwitchMessage(from, msg[0]);
        } else {
            handleSwitchMessage(from, msg[0], msg[1] - '0');
        }
        return;
    }

    if (msg.length() == 2 && msg[0] >= 'A' && msg[0] < 'A' + NLIGHTS) {
        handleRelayMessage(from, msg);
        return;
    }

    if (msg == "P") {
        mqttClient.publish(("/relay/state/" + String(from)).c_str(), "P");
        return;
    }

    if (isValidJson(msg)) {
        JsonDocument doc;
        deserializeJson(doc, msg.c_str());

        doc["parentId"] = nodeParentMap[from];

        String newMsg;
        serializeJson(doc, newMsg);

        if (WiFi.status() == WL_CONNECTED && mqttClient.connected())
            mqttMessageQueue.push({"/switch/state/root", newMsg});

        return;
    }
}

void checkWiFi() {
    IPAddress currentIP = getlocalIP();
    if (myIP != currentIP && currentIP != IPAddress(0, 0, 0, 0)) {
        myIP = currentIP;
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
            vTaskDelay(5000 / portTICK_PERIOD_MS);
        }

        if (!mqttClient.connected()) {
            unsigned long now = millis();
            if (now - lastMqttReconnect > MQTT_RECONNECT_INTERVAL) {
                lastMqttReconnect = now;
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

        auto nowNodes = mesh.getNodeList();

        // Remove disconnected nodes
        for (auto it = nodes.begin(); it != nodes.end();) {
            if (std::find(nowNodes.begin(), nowNodes.end(), it->first) ==
                nowNodes.end()) {
                it = nodes.erase(it);
            } else {
                ++it;
            }
        }

        // Query new nodes
        for (auto nodeId : nowNodes) {
            if (nodes.find(nodeId) == nodes.end()) {
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

void statusReport(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        painlessmesh::protocol::NodeTree layout = mesh.asNodeTree();
        nodeParentMap.clear();
        buildParentMap(layout);

        JsonDocument doc;
        doc["rssi"] = WiFi.RSSI();
        doc["uptime"] = millis() / 1000;
        doc["freeHeap"] = ESP.getFreeHeap();
        doc["deviceId"] = device_id;
        doc["parentId"] = device_id;
        doc["type"] = "root";
        doc["firmware"] = fw_md5;
        doc["clicks"] = 0;
        doc["disconnects"] = 0;

        String msg;
        serializeJson(doc, msg);

        if (WiFi.status() == WL_CONNECTED && mqttClient.connected())
            mqttMessageQueue.push({"/switch/state/root", msg});

        vTaskDelay(pdMS_TO_TICKS(STATUS_REPORT_INTERVAL));
    }
}

void meshUpdateTask(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        mesh.update();
        vTaskDelay(pdMS_TO_TICKS(1));  // Reduced delay for faster processing
    }
}

void sendMQTTMessages(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (mqttClient.connected() && !mqttMessageQueue.empty()) {
            std::pair<String, String> message = mqttMessageQueue.front();
            mqttMessageQueue.pop();

            mqttClient.publish(message.first.c_str(), message.second.c_str(),
                               message.second.length());
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

void setup() {
    Serial.begin(115200);
    vTaskDelay(500 / portTICK_PERIOD_MS);  // Reduced startup delay

    fw_md5 = ESP.getSketchMD5();
    DEBUG_INFO("Mesh Root Starting...");

    meshInit();

    // Optimized task priorities and stack sizes
    xTaskCreatePinnedToCore(checkWiFiAndMQTT, "WiFiMQTT", 8192, NULL, 2, NULL,
                            1);
    xTaskCreatePinnedToCore(statusReport, "Status", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(sendMQTTMessages, "sendMQTT", 4096, NULL, 2, NULL,
                            1);
    xTaskCreatePinnedToCore(checkMesh, "MeshCheck", 8192, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(meshUpdateTask, "MeshUpdate", 8192, NULL, 5, NULL,
                            0);
}

void loop() {
    static bool otaTaskStarted = false;

    if (otaInProgress && !otaTaskStarted) {
        otaTaskStarted = true;
        xTaskCreatePinnedToCore(otaTask, "OTA", 16384, NULL, 5, NULL, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
}