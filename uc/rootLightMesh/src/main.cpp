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
#include "esp_wifi.h"

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
#define DEBUG_ERROR(fmt, ...) Serial.printf("[ERROR] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_ERROR(fmt, ...)
#endif

#if DEBUG_LEVEL >= 2
#define DEBUG_INFO(fmt, ...) Serial.printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_INFO(fmt, ...)
#endif

#if DEBUG_LEVEL >= 3
#define DEBUG_VERBOSE(fmt, ...) \
    Serial.printf("[VERBOSE] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_VERBOSE(fmt, ...)
#endif

const int mqtt_port = 1883;
uint32_t device_id;

std::queue<std::pair<String, String>> mqttMessageQueue;
std::queue<std::pair<String, String>> mqttCallbackQueue;
std::queue<std::pair<uint32_t, String>> meshMessageQueue;
std::queue<std::pair<uint32_t, String>> meshCallbackQueue;
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
    DEBUG_INFO("OTA task started");
    otaInProgress = true;

    esp_task_wdt_deinit();
    DEBUG_VERBOSE("Watchdog disabled for OTA");

    mqttClient.disconnect();
    DEBUG_VERBOSE("MQTT disconnected");

    mesh.stop();
    DEBUG_VERBOSE("Mesh stopped");

    vTaskDelay(pdMS_TO_TICKS(1000));
    performFirmwareUpdate();
}

void performFirmwareUpdate() {
    const int MAX_RETRIES = 3;
    int attemptCount = 0;
    bool updateSuccess = false;

    while (attemptCount < MAX_RETRIES && !updateSuccess) {
        attemptCount++;
        DEBUG_INFO("OTA: Starting update attempt %d/%d...", attemptCount,
                   MAX_RETRIES);

        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        DEBUG_VERBOSE("OTA: Connecting to WiFi...");
        unsigned long startTime = millis();

        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - startTime > WIFI_CONNECT_TIMEOUT) {
                DEBUG_ERROR("OTA: WiFi timeout on attempt %d", attemptCount);
                break;
            }
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }

        // Check if WiFi connection failed
        if (WiFi.status() != WL_CONNECTED) {
            if (attemptCount < MAX_RETRIES) {
                DEBUG_INFO("OTA: Retrying in 2 seconds...");
                vTaskDelay(2000 / portTICK_PERIOD_MS);
                continue;
            } else {
                DEBUG_ERROR("OTA: All WiFi connection attempts failed");
                break;
            }
        }

        DEBUG_VERBOSE("OTA: WiFi connected, IP: %s",
                      WiFi.localIP().toString().c_str());

        WiFiClientSecure* client = new WiFiClientSecure();
        if (!client) {
            DEBUG_ERROR("OTA: Client alloc failed on attempt %d", attemptCount);
            if (attemptCount < MAX_RETRIES) {
                vTaskDelay(2000 / portTICK_PERIOD_MS);
                continue;
            }
            break;
        }

        client->setInsecure();
        HTTPClient http;
        http.setTimeout(30000);

        if (!http.begin(*client, firmware_url)) {
            DEBUG_ERROR("OTA: HTTP begin failed on attempt %d", attemptCount);
            delete client;
            if (attemptCount < MAX_RETRIES) {
                vTaskDelay(2000 / portTICK_PERIOD_MS);
                continue;
            }
            break;
        }

        DEBUG_VERBOSE("OTA: Downloading from %s", firmware_url);
        int httpCode = http.GET();

        if (httpCode == HTTP_CODE_OK) {
            int contentLength = http.getSize();
            DEBUG_VERBOSE("OTA: Content length: %d bytes", contentLength);

            if (contentLength <= 0 || !Update.begin(contentLength)) {
                DEBUG_ERROR("OTA: Invalid size or begin failed on attempt %d",
                            attemptCount);
                http.end();
                delete client;
                if (attemptCount < MAX_RETRIES) {
                    vTaskDelay(2000 / portTICK_PERIOD_MS);
                    continue;
                }
                break;
            }

            WiFiClient* stream = http.getStreamPtr();
            size_t written = Update.writeStream(*stream);
            DEBUG_VERBOSE("OTA: Written %d/%d bytes", written, contentLength);

            if (written != contentLength) {
                DEBUG_ERROR(
                    "OTA: Write mismatch on attempt %d (written: %d, expected: "
                    "%d)",
                    attemptCount, written, contentLength);
                Update.abort();
                http.end();
                delete client;
                if (attemptCount < MAX_RETRIES) {
                    vTaskDelay(2000 / portTICK_PERIOD_MS);
                    continue;
                }
                break;
            }

            if (Update.end() && Update.isFinished()) {
                DEBUG_INFO("OTA: Update successful on attempt %d!",
                           attemptCount);
                updateSuccess = true;
                http.end();
                delete client;
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                ESP.restart();
                return;
            } else {
                DEBUG_ERROR(
                    "OTA: Update.end() failed on attempt %d (error: %s)",
                    attemptCount, Update.errorString());
                http.end();
                delete client;
                if (attemptCount < MAX_RETRIES) {
                    vTaskDelay(2000 / portTICK_PERIOD_MS);
                    continue;
                }
            }
        } else {
            DEBUG_ERROR("OTA: HTTP failed with code %d on attempt %d", httpCode,
                        attemptCount);
            http.end();
            delete client;
            if (attemptCount < MAX_RETRIES) {
                vTaskDelay(2000 / portTICK_PERIOD_MS);
                continue;
            }
        }
    }

    // If we get here, all attempts failed
    DEBUG_ERROR("OTA: All %d update attempts failed. Restarting...",
                MAX_RETRIES);
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
    if (root.isNull()) {
        DEBUG_ERROR("parseConnections: root is null");
        return;
    }

    DEBUG_INFO("Parsing connections configuration");

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

    DEBUG_VERBOSE("Connections parsed:");
    for (const auto& idPair : connections) {
        DEBUG_VERBOSE("  ID: %s", idPair.first.c_str());
        for (const auto& letterPair : idPair.second) {
            DEBUG_VERBOSE("    Letter: %c", letterPair.first);
            for (const auto& target : letterPair.second) {
                DEBUG_VERBOSE("      -> %s: %s", target.first.c_str(),
                              target.second.c_str());
            }
        }
    }
}

void mqttConnect() {
    if (WiFi.status() != WL_CONNECTED) {
        DEBUG_VERBOSE("mqttConnect: WiFi not connected");
        return;
    }

    DEBUG_INFO("Connecting to MQTT broker...");

    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(90);
    mqttClient.setSocketTimeout(30);

    int retries = 0;
    while (!mqttClient.connected() && retries < MQTT_CONNECT_TIMEOUT) {
        String clientId = String(device_id);

        if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
            DEBUG_INFO("MQTT connected");
            mqttClient.subscribe("/switch/cmd/+");
            mqttClient.subscribe("/switch/cmd");
            mqttClient.subscribe("/relay/cmd/+");
            mqttClient.subscribe("/relay/cmd");
            DEBUG_VERBOSE("MQTT subscriptions completed");

            mqttMessageQueue.push({"/switch/state/root", "connected"});
            return;
        }
        retries++;
        DEBUG_VERBOSE("MQTT connection attempt %d/%d failed", retries,
                      MQTT_CONNECT_TIMEOUT);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    DEBUG_ERROR("MQTT connection failed after %d attempts",
                MQTT_CONNECT_TIMEOUT);
}

void meshInit() {
    DEBUG_INFO("Initializing mesh network...");

    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA);

    esp_wifi_set_ps(WIFI_PS_NONE);
    DEBUG_VERBOSE("WiFi power save disabled");

    mesh.stationManual(WIFI_SSID, WIFI_PASSWORD);
    mesh.setRoot(true);
    mesh.setContainsRoot(true);
    mesh.setHostname(HOSTNAME);

    mesh.onReceive(&receivedCallback);
    mesh.onDroppedConnection(&droppedConnectionCallback);
    mesh.onNewConnection(&newConnectionCallback);

    device_id = mesh.getNodeId();
    DEBUG_INFO("Mesh initialized, device ID: %u", device_id);
}

void droppedConnectionCallback(uint32_t nodeId) {
    DEBUG_INFO("Node disconnected: %u", nodeId);
    nodes.erase(nodeId);
}

void newConnectionCallback(uint32_t nodeId) {
    DEBUG_INFO("New node connected: %u", nodeId);
    meshMessageQueue.push({nodeId, "Q"});
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg;
    msg.reserve(length + 1);
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    DEBUG_VERBOSE("MQTT RX: [%s] %s", topic, msg.c_str());
    mqttCallbackQueue.push({String(topic), msg});
}

void receivedCallback(const uint32_t& from, const String& msg) {
    DEBUG_VERBOSE("MESH RX: [%u] %s", from, msg.c_str());
    meshCallbackQueue.push({from, msg});
}

void handleSwitchMessage(const uint32_t& from, const char output,
                         int state = -1) {
    std::vector<std::pair<String, String>> targets =
        connections[String(from)][output];

    DEBUG_INFO("SWITCH: Handling message from %u for output %c (state: %d)",
               from, output, state);

    for (const auto& target : targets) {
        String relayIdStr = target.first;
        String command = target.second;
        uint32_t relayId = relayIdStr.toInt();
        DEBUG_VERBOSE("SWITCH: Sending command '%s' to relay %s (%u)",
                      command.c_str(), relayIdStr.c_str(), relayId);

        if (state != -1) {
            command += String(state);
            meshMessageQueue.push({relayId, command});
        } else {
            meshMessageQueue.push({relayId, command});
        }
    }
}

void handleRelayMessage(const uint32_t& from, const String& msg) {
    if (WiFi.status() != WL_CONNECTED || !mqttClient.connected()) {
        DEBUG_VERBOSE("handleRelayMessage: WiFi or MQTT not connected");
        return;
    }

    String topic = "/relay/state/" + String(from);
    DEBUG_VERBOSE("RELAY: Publishing state from %u: %s", from, msg.c_str());
    mqttMessageQueue.push({topic, msg});
}

void checkWiFi() {
    IPAddress currentIP = getlocalIP();
    if (myIP != currentIP && currentIP != IPAddress(0, 0, 0, 0)) {
        myIP = currentIP;
        DEBUG_INFO("Connected to WiFi, IP: %s", myIP.toString().c_str());
    }
}

void checkWiFiAndMQTT(void* pvParameters) {
    DEBUG_VERBOSE("checkWiFiAndMQTT task started");

    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        checkWiFi();

        while (!WiFi.isConnected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (!mqttClient.connected()) {
            unsigned long now = millis();

            if (now - lastMqttReconnect > MQTT_RECONNECT_INTERVAL) {
                lastMqttReconnect = now;
                DEBUG_VERBOSE("Attempting MQTT reconnection");
                mqttConnect();
            }
        } else {
            mqttClient.loop();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void checkMesh(void* pvParameters) {
    DEBUG_VERBOSE("checkMesh task started");

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
                DEBUG_VERBOSE("Removing disconnected node: %u", it->first);
                it = nodes.erase(it);
            } else {
                ++it;
            }
        }

        // Query new nodes
        for (auto nodeId : nowNodes) {
            if (nodes.find(nodeId) == nodes.end()) {
                DEBUG_VERBOSE("Querying new node: %u", nodeId);
                meshMessageQueue.push({nodeId, "Q"});
                vTaskDelay(pdMS_TO_TICKS(25));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void buildParentMap(const painlessmesh::protocol::NodeTree& node,
                    uint32_t parent = 0) {
    nodeParentMap[node.nodeId] = parent;
    DEBUG_VERBOSE("buildParentMap: node %u parent %u", node.nodeId, parent);
    for (const auto& child : node.subs) {
        buildParentMap(child, node.nodeId);
    }
}

void statusReport(void* pvParameters) {
    DEBUG_VERBOSE("statusReport task started");

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

        DEBUG_VERBOSE("Status report: %s", msg.c_str());

        if (WiFi.status() == WL_CONNECTED && mqttClient.connected())
            mqttMessageQueue.push({"/switch/state/root", msg});
        else
            DEBUG_VERBOSE("Status report: WiFi or MQTT not connected");

        vTaskDelay(pdMS_TO_TICKS(STATUS_REPORT_INTERVAL));
    }
}

void handleUpdateMessage(const String& topic) {
    DEBUG_INFO("Update requested for topic: %s", topic.c_str());

    if (topic == "/switch/cmd/root") {
        DEBUG_INFO("Root OTA update triggered");
        otaInProgress = true;
        return;
    }

    String path = topic;
    int lastSlash = path.lastIndexOf('/');
    String last = path.substring(lastSlash + 1);
    uint64_t nodeId;

    if (toUint64(last, nodeId)) {
        DEBUG_INFO("Update requested for node: %llu", nodeId);
        meshMessageQueue.push({(uint32_t)nodeId, "U"});
        return;
    }

    DEBUG_INFO("Broadcasting update to all compatible nodes");
    for (const auto& pair : nodes) {
        uint32_t nodeId = pair.first;
        String nodeType = pair.second;

        if ((nodeType == "relay" && topic == "/switch/cmd") ||
            (nodeType == "switch" && topic == "/relay/cmd")) {
            continue;
        }
        DEBUG_VERBOSE("Sending update to node %u (%s)", nodeId,
                      nodeType.c_str());
        meshMessageQueue.push({nodeId, "U"});
    }
}

void sendMQTTMessages(void* pvParameters) {
    DEBUG_VERBOSE("sendMQTTMessages task started");

    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!mqttClient.connected() || mqttMessageQueue.empty()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(5));

        std::pair<String, String> message = mqttMessageQueue.front();
        String topic = message.first;
        String msg = message.second;
        mqttMessageQueue.pop();

        DEBUG_VERBOSE("MQTT TX: [%s] %s", topic.c_str(), msg.c_str());
        mqttClient.publish(topic.c_str(), msg.c_str(), msg.length());
    }
}

void sendMeshMessages(void* pvParameters) {
    DEBUG_VERBOSE("sendMeshMessages task started");

    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (meshMessageQueue.empty()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(5));

        auto message = meshMessageQueue.front();
        meshMessageQueue.pop();

        uint32_t to = message.first;
        String msg = message.second;

        DEBUG_VERBOSE("MESH TX: [%u] %s", to, msg.c_str());
        mesh.sendSingle(to, msg);
    }
}

void mqttCallbackTask(void* pvParameters) {
    DEBUG_VERBOSE("mqttCallbackTask started");

    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!mqttClient.connected()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (mqttCallbackQueue.empty()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(5));

        std::pair<String, String> message = mqttCallbackQueue.front();
        mqttCallbackQueue.pop();

        String topic = message.first;
        String msg = message.second;

        if (msg == "U") {
            handleUpdateMessage(topic);
            continue;
        }
        if (topic == "/switch/cmd/root" && isValidJson(msg)) {
            DEBUG_INFO("Received connections config");
            JsonDocument doc;
            deserializeJson(doc, msg);

            parseConnections(doc.as<JsonObject>());

            continue;
        }
        size_t lastSlash = topic.lastIndexOf('/');
        if (lastSlash != -1 && lastSlash < topic.length() - 1) {
            String idStr = topic.substring(lastSlash + 1);
            uint32_t nodeId = idStr.toInt();

            if (nodeId != 0 || idStr == "0") {
                if (nodes.count(nodeId)) {
                    DEBUG_VERBOSE("Forwarding command to node %u: %s", nodeId,
                                  msg.c_str());
                    meshMessageQueue.push({nodeId, msg});
                } else {
                    DEBUG_VERBOSE("Node %u not found in node list", nodeId);
                }
            }
        }
    }
}

void meshCallbackTask(void* pvParameters) {
    DEBUG_VERBOSE("meshCallbackTask started");

    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (meshCallbackQueue.empty()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(5));

        std::pair<uint32_t, String> message = meshCallbackQueue.front();
        uint32_t from = message.first;
        String msg = message.second;
        meshCallbackQueue.pop();

        if (msg == "R") {
            DEBUG_INFO("Node %u identified as relay", from);
            nodes[from] = "relay";
            meshMessageQueue.push({from, "A"});
            continue;
        }

        if (msg == "S") {
            DEBUG_INFO("Node %u identified as switch", from);
            nodes[from] = "switch";
            meshMessageQueue.push({from, "A"});
            continue;
        }

        if (msg[0] >= 'a' && msg[0] < 'a' + NLIGHTS) {
            if (msg.length() == 1) {
                handleSwitchMessage(from, msg[0]);
            } else {
                handleSwitchMessage(from, msg[0], msg[1] - '0');
            }
            continue;
        }

        if (msg.length() == 2 && msg[0] >= 'A' && msg[0] < 'A' + NLIGHTS) {
            handleRelayMessage(from, msg);
            continue;
        }

        if (isValidJson(msg)) {
            DEBUG_VERBOSE("Processing JSON status from node %u", from);
            JsonDocument doc;
            deserializeJson(doc, msg.c_str());

            doc["parentId"] = nodeParentMap[from];

            String newMsg;
            serializeJson(doc, newMsg);

            if (WiFi.status() == WL_CONNECTED && mqttClient.connected())
                mqttMessageQueue.push({"/switch/state/root", newMsg});
            else
                DEBUG_VERBOSE(
                    "Cannot publish status: WiFi or MQTT not connected");

            continue;
        }

        DEBUG_VERBOSE("Unknown message from node %u: %s", from, msg.c_str());
    }
}

void setup() {
    Serial.begin(115200);
    vTaskDelay(500 / portTICK_PERIOD_MS);

    fw_md5 = ESP.getSketchMD5();
    DEBUG_INFO("=== Mesh Root Starting ===");
    DEBUG_INFO("Firmware MD5: %s", fw_md5.c_str());
    DEBUG_INFO("Free heap: %u bytes", ESP.getFreeHeap());

    meshInit();

    DEBUG_INFO("Creating tasks...");
    xTaskCreatePinnedToCore(checkWiFiAndMQTT, "WiFiMQTT", 8192, NULL, 2, NULL,
                            1);
    xTaskCreatePinnedToCore(statusReport, "Status", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(checkMesh, "MeshCheck", 8192, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(sendMQTTMessages, "sendMQTT", 4096, NULL, 2, NULL,
                            1);
    xTaskCreatePinnedToCore(sendMeshMessages, "sendMesh", 4096, NULL, 2, NULL,
                            0);
    xTaskCreatePinnedToCore(mqttCallbackTask, "MQTTCallback", 8192, NULL, 4,
                            NULL, 1);
    xTaskCreatePinnedToCore(meshCallbackTask, "MeshCallback", 8192, NULL, 4,
                            NULL, 0);

    DEBUG_INFO("=== Initialization complete ===");
}

void loop() {
    static bool otaTaskStarted = false;

    if (otaInProgress && !otaTaskStarted) {
        otaTaskStarted = true;
        DEBUG_INFO("Starting OTA task");
        xTaskCreatePinnedToCore(otaTask, "OTA", 16384, NULL, 5, NULL, 0);
    }

    if (otaInProgress) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
        mesh.update();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}