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
#define STATUS_REPORT_INTERVAL 15000
#define MQTT_RECONNECT_INTERVAL 30000
#define WIFI_CONNECT_TIMEOUT 20000
#define MQTT_CONNECT_TIMEOUT 5
#define MAX_QUEUE_SIZE 50
#define CRITICAL_HEAP_THRESHOLD 30000
#define LOW_HEAP_THRESHOLD 50000
#define DEBUG_LEVEL 1

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
std::queue<std::pair<uint32_t, String>>
    meshPriorityQueue;  // NEW: Fast path for relay commands
std::queue<std::pair<uint32_t, String>> meshCallbackQueue;
std::map<uint32_t, String> nodes;
std::map<String, std::map<char, std::vector<std::pair<String, String>>>>
    connections;
std::map<uint32_t, uint32_t> nodeParentMap;

SemaphoreHandle_t mqttMessageQueueMutex = NULL;
SemaphoreHandle_t mqttCallbackQueueMutex = NULL;
SemaphoreHandle_t meshMessageQueueMutex = NULL;
SemaphoreHandle_t meshPriorityQueueMutex = NULL;  // NEW
SemaphoreHandle_t meshCallbackQueueMutex = NULL;
SemaphoreHandle_t nodesMapMutex = NULL;
SemaphoreHandle_t connectionsMapMutex = NULL;

struct Statistics {
    uint32_t mqttDropped = 0;
    uint32_t meshDropped = 0;
    uint32_t lowHeapEvents = 0;
    uint32_t criticalHeapEvents = 0;
} stats;

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
volatile bool otaInProgress = false;
String fw_md5;

template <typename T>
bool safePush(std::queue<T>& q, const T& item, SemaphoreHandle_t mutex,
              uint32_t& dropCounter, const char* queueName) {
    if (!mutex) {
        DEBUG_ERROR("Mutex is NULL for %s queue - dropping message", queueName);
        dropCounter++;
        return false;
    }
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (q.size() >= MAX_QUEUE_SIZE) {
            dropCounter++;
            DEBUG_ERROR(
                "%s queue full (%d items), dropping message (total dropped: "
                "%u)",
                queueName, q.size(), dropCounter);
            xSemaphoreGive(mutex);
            return false;
        }
        q.push(item);
        xSemaphoreGive(mutex);
        return true;
    }
    DEBUG_ERROR("Failed to acquire mutex for %s queue", queueName);
    dropCounter++;
    return false;
}

template <typename T>
bool safePop(std::queue<T>& q, T& item, SemaphoreHandle_t mutex) {
    if (!mutex) {
        return false;
    }
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (q.empty()) {
            xSemaphoreGive(mutex);
            return false;
        }
        item = q.front();
        q.pop();
        xSemaphoreGive(mutex);
        return true;
    }
    return false;
}

bool checkHeapHealth() {
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < CRITICAL_HEAP_THRESHOLD) {
        stats.criticalHeapEvents++;
        DEBUG_ERROR("CRITICAL: Low heap %u bytes! Clearing queues...",
                    freeHeap);
        if (xSemaphoreTake(mqttMessageQueueMutex, pdMS_TO_TICKS(100)) ==
            pdTRUE) {
            while (!mqttMessageQueue.empty()) mqttMessageQueue.pop();
            xSemaphoreGive(mqttMessageQueueMutex);
        }
        return false;
    } else if (freeHeap < LOW_HEAP_THRESHOLD) {
        stats.lowHeapEvents++;
        DEBUG_ERROR("Low heap: %u bytes", freeHeap);
        return false;
    }
    return true;
}

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
    esp_task_wdt_init(30, true);
    DEBUG_ERROR("OTA failed, watchdog re-enabled");
    otaInProgress = false;
    vTaskDelete(NULL);
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
    if (xSemaphoreTake(connectionsMapMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        DEBUG_ERROR("Failed to acquire connections mutex");
        return;
    }
    connections.clear();
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
    xSemaphoreGive(connectionsMapMutex);
}

void sendConnectionToNode(uint32_t nodeId) {
    String nodeIdStr = String(nodeId);

    // Check if this node has connections
    if (xSemaphoreTake(connectionsMapMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        DEBUG_ERROR(
            "sendConnectionToNode: Failed to acquire connections mutex");
        return;
    }

    auto it = connections.find(nodeIdStr);
    if (it == connections.end()) {
        xSemaphoreGive(connectionsMapMutex);
        DEBUG_VERBOSE("No connections configured for node %u", nodeId);
        return;
    }

    // Build JSON for this specific node
    JsonDocument doc;
    JsonObject nodeObj = doc[nodeIdStr].to<JsonObject>();

    for (const auto& letterPair : it->second) {
        char letter = letterPair.first;
        JsonArray letterArray = nodeObj[String(letter)].to<JsonArray>();

        for (const auto& target : letterPair.second) {
            JsonArray targetPair = letterArray.add<JsonArray>();
            targetPair.add(target.first);
            targetPair.add(target.second);
        }
    }

    String jsonStr;
    serializeJson(doc, jsonStr);
    xSemaphoreGive(connectionsMapMutex);

    // Send to this specific node
    safePush(meshMessageQueue, std::make_pair(nodeId, jsonStr),
             meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
    DEBUG_INFO("Sent connections to node %u: %s", nodeId, jsonStr.c_str());
}

void sendConnectionsToAllNodes() {
    // First, get list of all node IDs while holding nodes mutex
    if (xSemaphoreTake(nodesMapMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        DEBUG_ERROR("sendConnectionsToAllNodes: Failed to acquire nodes mutex");
        return;
    }

    std::vector<uint32_t> nodeIds;
    for (const auto& node : nodes) {
        nodeIds.push_back(node.first);
    }
    xSemaphoreGive(nodesMapMutex);

    DEBUG_INFO("Sending connections to %d nodes", nodeIds.size());

    // Send to each node
    for (uint32_t nodeId : nodeIds) {
        sendConnectionToNode(nodeId);
        vTaskDelay(pdMS_TO_TICKS(50));  // Small delay to avoid flooding mesh
    }

    DEBUG_INFO("Finished sending connections to all nodes");
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
            safePush(mqttMessageQueue,
                     std::make_pair(String("/switch/state/root"),
                                    String("connected")),
                     mqttMessageQueueMutex, stats.mqttDropped, "MQTT-MSG");
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
    if (xSemaphoreTake(nodesMapMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        nodes.erase(nodeId);
        xSemaphoreGive(nodesMapMutex);
    }
}

void newConnectionCallback(uint32_t nodeId) {
    DEBUG_INFO("New node connected: %u", nodeId);
    safePush(meshMessageQueue, std::make_pair(nodeId, String("Q")),
             meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (!checkHeapHealth()) {
        stats.mqttDropped++;
        return;
    }
    String msg;
    msg.reserve(length + 1);
    msg.concat((char*)payload, length);
    DEBUG_VERBOSE("MQTT RX: [%s] %s", topic, msg.c_str());
    safePush(mqttCallbackQueue, std::make_pair(String(topic), msg),
             mqttCallbackQueueMutex, stats.mqttDropped, "MQTT-CB");
}

void receivedCallback(const uint32_t& from, const String& msg) {
    DEBUG_VERBOSE("MESH RX: [%u] %s", from, msg.c_str());
    safePush(meshCallbackQueue, std::make_pair(from, msg),
             meshCallbackQueueMutex, stats.meshDropped, "MESH-CB");
}

void handleSwitchMessage(const uint32_t& from, const char output,
                         int state = -1, bool priority = false) {
    if (xSemaphoreTake(connectionsMapMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        DEBUG_ERROR(
            "Failed to acquire connections mutex in handleSwitchMessage");
        return;
    }
    auto it = connections.find(String(from));
    if (it == connections.end()) {
        xSemaphoreGive(connectionsMapMutex);
        return;
    }
    auto& outputMap = it->second;
    auto outputIt = outputMap.find(output);
    if (outputIt == outputMap.end()) {
        xSemaphoreGive(connectionsMapMutex);
        return;
    }
    std::vector<std::pair<String, String>> targets = outputIt->second;
    xSemaphoreGive(connectionsMapMutex);
    DEBUG_INFO("SWITCH: Handling message from %u for output %c (state: %d)",
               from, output, state);
    for (const auto& target : targets) {
        String relayIdStr = target.first;
        String command = target.second;
        uint32_t relayId = relayIdStr.toInt();
        DEBUG_VERBOSE("SWITCH: Sending command '%s' to relay %s (%u)",
                      command.c_str(), relayIdStr.c_str(), relayId);
        if (state != -1) command += String(state);

        // Use priority queue for real-time commands
        if (priority) {
            safePush(meshPriorityQueue, std::make_pair(relayId, command),
                     meshPriorityQueueMutex, stats.meshDropped,
                     "MESH-PRIORITY");
        } else {
            safePush(meshMessageQueue, std::make_pair(relayId, command),
                     meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
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
    safePush(mqttMessageQueue, std::make_pair(topic, msg),
             mqttMessageQueueMutex, stats.mqttDropped, "MQTT-MSG");
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
        vTaskDelay(pdMS_TO_TICKS(50));
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
        if (xSemaphoreTake(nodesMapMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (auto it = nodes.begin(); it != nodes.end();) {
                if (std::find(nowNodes.begin(), nowNodes.end(), it->first) ==
                    nowNodes.end()) {
                    DEBUG_VERBOSE("Removing disconnected node: %u", it->first);
                    it = nodes.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto nodeId : nowNodes) {
                if (nodes.find(nodeId) == nodes.end()) {
                    DEBUG_VERBOSE("Querying new node: %u", nodeId);
                    xSemaphoreGive(nodesMapMutex);
                    safePush(
                        meshMessageQueue, std::make_pair(nodeId, String("Q")),
                        meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
                    vTaskDelay(pdMS_TO_TICKS(25));
                    xSemaphoreTake(nodesMapMutex, pdMS_TO_TICKS(100));
                }
            }
            xSemaphoreGive(nodesMapMutex);
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
        doc["mqttDropped"] = stats.mqttDropped;
        doc["meshDropped"] = stats.meshDropped;
        doc["lowHeap"] = stats.lowHeapEvents;
        doc["criticalHeap"] = stats.criticalHeapEvents;
        String msg;
        serializeJson(doc, msg);
        DEBUG_VERBOSE("Status report: %s", msg.c_str());
        if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
            safePush(mqttMessageQueue,
                     std::make_pair(String("/switch/state/root"), msg),
                     mqttMessageQueueMutex, stats.mqttDropped, "MQTT-MSG");
        } else {
            DEBUG_VERBOSE("Status report: WiFi or MQTT not connected");
        }
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
        safePush(meshMessageQueue,
                 std::make_pair((uint32_t)nodeId, String("U")),
                 meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
        return;
    }
    DEBUG_INFO("Broadcasting update to all compatible nodes");
    if (xSemaphoreTake(nodesMapMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto& pair : nodes) {
            uint32_t nodeId = pair.first;
            String nodeType = pair.second;
            if ((nodeType == "relay" && topic == "/switch/cmd") ||
                (nodeType == "switch" && topic == "/relay/cmd")) {
                continue;
            }
            DEBUG_VERBOSE("Sending update to node %u (%s)", nodeId,
                          nodeType.c_str());
            xSemaphoreGive(nodesMapMutex);
            safePush(meshMessageQueue, std::make_pair(nodeId, String("U")),
                     meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
            xSemaphoreTake(nodesMapMutex, pdMS_TO_TICKS(100));
        }
        xSemaphoreGive(nodesMapMutex);
    }
}

void sendMQTTMessages(void* pvParameters) {
    DEBUG_VERBOSE("sendMQTTMessages task started");
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!mqttClient.connected()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        std::pair<String, String> message;
        if (!safePop(mqttMessageQueue, message, mqttMessageQueueMutex)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        String topic = message.first;
        String msg = message.second;
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

        // Check priority queue FIRST (no delay for urgent commands)
        std::pair<uint32_t, String> message;
        if (safePop(meshPriorityQueue, message, meshPriorityQueueMutex)) {
            uint32_t to = message.first;
            String msg = message.second;
            DEBUG_VERBOSE("MESH TX PRIORITY: [%u] %s", to, msg.c_str());
            mesh.sendSingle(to, msg);
            vTaskDelay(pdMS_TO_TICKS(2));  // Minimal delay for priority
            continue;
        }

        // Then check regular queue
        if (!safePop(meshMessageQueue, message, meshMessageQueueMutex)) {
            vTaskDelay(pdMS_TO_TICKS(5));  // Reduced from 20ms
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
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
        std::pair<String, String> message;
        if (!safePop(mqttCallbackQueue, message, mqttCallbackQueueMutex)) {
            vTaskDelay(pdMS_TO_TICKS(5));  // Reduced from 20ms
            continue;
        }
        // NO DELAY HERE - process immediately for relay commands
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

            sendConnectionsToAllNodes();
            continue;
        }

        // Fast path for relay commands
        bool isRelayCommand = topic.startsWith("/relay/cmd/");

        size_t lastSlash = topic.lastIndexOf('/');
        if (lastSlash != -1 && lastSlash < topic.length() - 1) {
            String idStr = topic.substring(lastSlash + 1);
            uint32_t nodeId = idStr.toInt();
            if (nodeId != 0 || idStr == "0") {
                if (xSemaphoreTake(nodesMapMutex, pdMS_TO_TICKS(50)) !=
                    pdTRUE) {
                    DEBUG_ERROR("Failed to acquire nodes mutex");
                    continue;
                }

                bool found = (nodes.count(nodeId) > 0);
                xSemaphoreGive(nodesMapMutex);
                if (!found) {
                    DEBUG_VERBOSE("Node %u not found in node list", nodeId);
                    continue;
                }

                DEBUG_VERBOSE("Forwarding command to node %u: %s", nodeId,
                              msg.c_str());
                // Use priority queue for relay commands from MQTT
                if (isRelayCommand) {
                    safePush(meshPriorityQueue, std::make_pair(nodeId, msg),
                             meshPriorityQueueMutex, stats.meshDropped,
                             "MESH-PRIORITY");
                } else {
                    safePush(meshMessageQueue, std::make_pair(nodeId, msg),
                             meshMessageQueueMutex, stats.meshDropped,
                             "MESH-MSG");
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
        std::pair<uint32_t, String> message;
        if (!safePop(meshCallbackQueue, message, meshCallbackQueueMutex)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
        uint32_t from = message.first;
        String msg = message.second;

        if (msg == "R") {
            DEBUG_INFO("Node %u identified as relay", from);
            if (xSemaphoreTake(nodesMapMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                nodes[from] = "relay";
                xSemaphoreGive(nodesMapMutex);
            }
            safePush(meshMessageQueue, std::make_pair(from, String("A")),
                     meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");

            sendConnectionToNode(from);
            continue;
        }

        if (msg == "S") {
            DEBUG_INFO("Node %u identified as switch", from);
            if (xSemaphoreTake(nodesMapMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                nodes[from] = "switch";
                xSemaphoreGive(nodesMapMutex);
            }
            safePush(meshMessageQueue, std::make_pair(from, String("A")),
                     meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");

            sendConnectionToNode(from);
            continue;
        }

        if (msg[0] >= 'a' && msg[0] < 'a' + NLIGHTS) {
            if (msg.length() == 1) {
                handleSwitchMessage(from, msg[0]);
            } else if (msg.length() == 2 && msg[1] >= '0' && msg[1] <= '1') {
                handleSwitchMessage(from, msg[0], msg[1] - '0');
            }

            safePush(mqttMessageQueue,
                     std::make_pair(
                         String(String("/switch/state/") + String(from)), msg),
                     mqttMessageQueueMutex, stats.mqttDropped, "MQTT-MSG");

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
            if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
                safePush(mqttMessageQueue,
                         std::make_pair(String("/switch/state/root"), newMsg),
                         mqttMessageQueueMutex, stats.mqttDropped, "MQTT-MSG");
            } else {
                DEBUG_VERBOSE(
                    "Cannot publish status: WiFi or MQTT not connected");
            }
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

    // CRITICAL: Create mutexes BEFORE meshInit() because callbacks can fire
    // immediately
    DEBUG_INFO("Creating mutexes...");
    mqttMessageQueueMutex = xSemaphoreCreateMutex();
    mqttCallbackQueueMutex = xSemaphoreCreateMutex();
    meshMessageQueueMutex = xSemaphoreCreateMutex();
    meshPriorityQueueMutex = xSemaphoreCreateMutex();
    meshCallbackQueueMutex = xSemaphoreCreateMutex();
    nodesMapMutex = xSemaphoreCreateMutex();
    connectionsMapMutex = xSemaphoreCreateMutex();

    // Verify all mutexes were created
    if (!mqttMessageQueueMutex || !mqttCallbackQueueMutex ||
        !meshMessageQueueMutex || !meshPriorityQueueMutex ||
        !meshCallbackQueueMutex || !nodesMapMutex || !connectionsMapMutex) {
        DEBUG_ERROR("FATAL: Failed to create mutexes!");
        ESP.restart();
    }
    DEBUG_INFO("All mutexes created successfully");

    meshInit();
    DEBUG_INFO("Creating tasks...");
    xTaskCreatePinnedToCore(checkWiFiAndMQTT, "WiFiMQTT", 8192, NULL, 2, NULL,
                            1);
    xTaskCreatePinnedToCore(statusReport, "Status", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(checkMesh, "MeshCheck", 8192, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(sendMQTTMessages, "sendMQTT", 4096, NULL, 2, NULL,
                            1);
    xTaskCreatePinnedToCore(sendMeshMessages, "sendMesh", 4096, NULL, 3, NULL,
                            0);  // Higher priority
    xTaskCreatePinnedToCore(mqttCallbackTask, "MQTTCallback", 12288, NULL, 4,
                            NULL, 1);
    xTaskCreatePinnedToCore(meshCallbackTask, "MeshCallback", 12288, NULL, 4,
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