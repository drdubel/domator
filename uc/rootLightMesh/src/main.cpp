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
#include <esp_now.h>
#include <esp_wifi.h>

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

#define HOSTNAME "espnow_root"
#define NLIGHTS 8
#define STATUS_REPORT_INTERVAL 15000
#define MQTT_RECONNECT_INTERVAL 30000
#define WIFI_CONNECT_TIMEOUT 20000
#define MQTT_CONNECT_TIMEOUT 5
#define MAX_QUEUE_SIZE 50
#define CRITICAL_HEAP_THRESHOLD 30000
#define LOW_HEAP_THRESHOLD 50000
#define ESPNOW_CHANNEL 1
#define MAX_ESPNOW_PEERS 20

#define DEBUG_LEVEL 2

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

// ESP-NOW message structure
typedef struct __attribute__((packed)) {
    uint32_t nodeId;
    uint8_t msgType;  // 'Q'=query, 'R'=relay, 'S'=switch, 'A'=ack, 'U'=update,
                      // 'D'=data, 'C'=command
    char data[200];
} espnow_message_t;

// Peer info structure
typedef struct {
    uint8_t mac[6];
    uint32_t nodeId;
    String nodeType;  // "relay" or "switch"
    unsigned long lastSeen;
} peer_info_t;

std::queue<std::pair<String, String>> mqttMessageQueue;
std::queue<std::pair<String, String>> mqttCallbackQueue;
std::queue<std::pair<uint32_t, String>> espnowMessageQueue;
std::queue<std::pair<uint32_t, String>> espnowPriorityQueue;
std::queue<espnow_message_t> espnowCallbackQueue;
std::map<uint32_t, peer_info_t> peers;
std::map<String, std::map<char, std::vector<std::pair<String, String>>>>
    connections;

SemaphoreHandle_t mqttMessageQueueMutex = NULL;
SemaphoreHandle_t mqttCallbackQueueMutex = NULL;
SemaphoreHandle_t espnowMessageQueueMutex = NULL;
SemaphoreHandle_t espnowPriorityQueueMutex = NULL;
SemaphoreHandle_t espnowCallbackQueueMutex = NULL;
SemaphoreHandle_t peersMapMutex = NULL;
SemaphoreHandle_t connectionsMapMutex = NULL;

struct Statistics {
    uint32_t mqttDropped = 0;
    uint32_t espnowDropped = 0;
    uint32_t lowHeapEvents = 0;
    uint32_t criticalHeapEvents = 0;
    uint32_t espnowSendFailed = 0;
    uint32_t espnowSendSuccess = 0;
    uint32_t buttonPresses = 0;
    uint32_t commandsRouted = 0;
} stats;

WiFiClient wifiClient;
PubSubClient mqttClient(mqtt_broker, mqtt_port, wifiClient);

static unsigned long lastMqttReconnect = 0;
volatile bool otaInProgress = false;
String fw_md5;

// Forward declarations
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttConnect();
void espnowInit();
void performFirmwareUpdate();
void onESPNowDataSent(const uint8_t* mac_addr, esp_now_send_status_t status);
void onESPNowDataRecv(const uint8_t* mac_addr, const uint8_t* data, int len);

template <typename T>
bool safePush(std::queue<T>& q, const T& item, SemaphoreHandle_t mutex,
              uint32_t& dropCounter, const char* queueName) {
    if (!mutex) {
        DEBUG_ERROR("Mutex is NULL for %s queue", queueName);
        dropCounter++;
        return false;
    }
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (q.size() >= MAX_QUEUE_SIZE) {
            dropCounter++;
            DEBUG_ERROR("%s queue full (%d items), dropped %u total", queueName,
                        q.size(), dropCounter);
            xSemaphoreGive(mutex);
            return false;
        }
        q.push(item);
        xSemaphoreGive(mutex);
        return true;
    }
    DEBUG_ERROR("Failed to acquire mutex for %s", queueName);
    dropCounter++;
    return false;
}

template <typename T>
bool safePop(std::queue<T>& q, T& item, SemaphoreHandle_t mutex) {
    if (!mutex) return false;
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
        DEBUG_ERROR("CRITICAL: Low heap %u bytes!", freeHeap);
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
    mqttClient.disconnect();
    esp_now_deinit();
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
        DEBUG_INFO("OTA: Attempt %d/%d", attemptCount, MAX_RETRIES);

        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - startTime > WIFI_CONNECT_TIMEOUT) {
                DEBUG_ERROR("OTA: WiFi timeout");
                break;
            }
            vTaskDelay(500);
        }

        if (WiFi.status() != WL_CONNECTED) {
            if (attemptCount < MAX_RETRIES) {
                vTaskDelay(2000);
                continue;
            }
            break;
        }

        WiFiClientSecure* client = new WiFiClientSecure();
        if (!client) {
            if (attemptCount < MAX_RETRIES) {
                vTaskDelay(2000);
                continue;
            }
            break;
        }

        client->setInsecure();
        HTTPClient http;
        http.setTimeout(30000);

        if (!http.begin(*client, firmware_url)) {
            delete client;
            if (attemptCount < MAX_RETRIES) {
                vTaskDelay(2000);
                continue;
            }
            break;
        }

        int httpCode = http.GET();

        if (httpCode == HTTP_CODE_OK) {
            int contentLength = http.getSize();

            if (contentLength > 0 && Update.begin(contentLength)) {
                WiFiClient* stream = http.getStreamPtr();
                size_t written = Update.writeStream(*stream);

                if (written == contentLength && Update.end() &&
                    Update.isFinished()) {
                    DEBUG_INFO("OTA: Success!");
                    http.end();
                    delete client;
                    vTaskDelay(1000);
                    ESP.restart();
                    return;
                }
                Update.abort();
            }
        }

        http.end();
        delete client;

        if (attemptCount < MAX_RETRIES) {
            vTaskDelay(2000);
        }
    }

    DEBUG_ERROR("OTA: All attempts failed. Restarting...");
    vTaskDelay(1000);
    ESP.restart();
}

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
    DEBUG_INFO("Connections parsed successfully");
}

void sendConnectionToNode(uint32_t nodeId) {
    String nodeIdStr = String(nodeId);

    if (xSemaphoreTake(connectionsMapMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        DEBUG_ERROR("sendConnectionToNode: Failed to acquire mutex");
        return;
    }

    auto it = connections.find(nodeIdStr);
    if (it == connections.end()) {
        xSemaphoreGive(connectionsMapMutex);
        DEBUG_VERBOSE("No connections for node %u", nodeId);
        return;
    }

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

    safePush(espnowMessageQueue, std::make_pair(nodeId, jsonStr),
             espnowMessageQueueMutex, stats.espnowDropped, "ESPNOW-MSG");
    DEBUG_INFO("Sent connections to node %u", nodeId);
}

void sendConnectionsToAllNodes() {
    if (xSemaphoreTake(peersMapMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        DEBUG_ERROR("sendConnectionsToAllNodes: Failed to acquire mutex");
        return;
    }

    std::vector<uint32_t> nodeIds;
    for (const auto& peer : peers) {
        nodeIds.push_back(peer.first);
    }
    xSemaphoreGive(peersMapMutex);

    DEBUG_INFO("Sending connections to %d nodes", nodeIds.size());

    for (uint32_t nodeId : nodeIds) {
        sendConnectionToNode(nodeId);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void mqttConnect() {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    DEBUG_INFO("Connecting to MQTT...");
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

            safePush(mqttMessageQueue,
                     std::make_pair(String("/switch/state/root"),
                                    String("connected")),
                     mqttMessageQueueMutex, stats.mqttDropped, "MQTT-MSG");
            return;
        }
        retries++;
        vTaskDelay(1000);
    }

    DEBUG_ERROR("MQTT connection failed");
}

void onESPNowDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        stats.espnowSendSuccess++;
    } else {
        stats.espnowSendFailed++;
    }
}

void onESPNowDataRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
    if (len != sizeof(espnow_message_t)) {
        DEBUG_ERROR("ESP-NOW: Invalid size: %d", len);
        return;
    }

    espnow_message_t msg;
    memcpy(&msg, data, sizeof(espnow_message_t));

    if (!checkHeapHealth()) {
        stats.espnowDropped++;
        return;
    }

    // Update peer info
    if (xSemaphoreTake(peersMapMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        auto it = peers.find(msg.nodeId);
        if (it != peers.end()) {
            memcpy(it->second.mac, mac_addr, 6);
            it->second.lastSeen = millis();
        } else {
            peer_info_t newPeer;
            memcpy(newPeer.mac, mac_addr, 6);
            newPeer.nodeId = msg.nodeId;
            newPeer.lastSeen = millis();
            newPeer.nodeType = "unknown";
            peers[msg.nodeId] = newPeer;
        }
        xSemaphoreGive(peersMapMutex);
    }

    safePush(espnowCallbackQueue, msg, espnowCallbackQueueMutex,
             stats.espnowDropped, "ESPNOW-CB");
}

void espnowInit() {
    DEBUG_INFO("Initializing ESP-NOW...");

    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        vTaskDelay(500);
        retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        DEBUG_INFO("WiFi connected: %s", WiFi.localIP().toString().c_str());
    }

    wifi_ap_record_t ap_info;
    uint8_t channel = ESPNOW_CHANNEL;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        channel = ap_info.primary;
        DEBUG_INFO("WiFi channel: %d", channel);
    }

    if (esp_now_init() != ESP_OK) {
        DEBUG_ERROR("ESP-NOW init failed");
        return;
    }

    DEBUG_INFO("ESP-NOW initialized");

    esp_now_register_send_cb(onESPNowDataSent);
    esp_now_register_recv_cb(onESPNowDataRecv);

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    device_id = (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5];

    DEBUG_INFO("Device ID: %u", device_id);
    DEBUG_INFO("Root MAC: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
               mac[2], mac[3], mac[4], mac[5]);
}

bool addESPNowPeer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) {
        return true;
    }

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        return false;
    }

    return true;
}

void sendESPNowMessage(uint32_t nodeId, const String& message,
                       bool priority = false) {
    if (xSemaphoreTake(peersMapMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    auto it = peers.find(nodeId);
    if (it == peers.end()) {
        xSemaphoreGive(peersMapMutex);
        return;
    }

    espnow_message_t msg;
    msg.nodeId = device_id;
    msg.msgType = 'D';

    if (message == "Q") {
        msg.msgType = 'Q';
    } else if (message == "A") {
        msg.msgType = 'A';
    } else if (message == "U") {
        msg.msgType = 'U';
    } else if (message.length() > 0 && message.length() < sizeof(msg.data)) {
        msg.msgType = 'C';
    }

    strncpy(msg.data, message.c_str(), sizeof(msg.data) - 1);
    msg.data[sizeof(msg.data) - 1] = '\0';

    uint8_t mac[6];
    memcpy(mac, it->second.mac, 6);

    xSemaphoreGive(peersMapMutex);

    addESPNowPeer(mac);

    esp_err_t result =
        esp_now_send(mac, (uint8_t*)&msg, sizeof(espnow_message_t));

    if (result != ESP_OK) {
        stats.espnowSendFailed++;
    }
}

void handleRelayMessage(const uint32_t& from, const String& msg) {
    if (WiFi.status() != WL_CONNECTED || !mqttClient.connected()) {
        return;
    }

    String topic = "/relay/state/" + String(from);

    safePush(mqttMessageQueue, std::make_pair(topic, msg),
             mqttMessageQueueMutex, stats.mqttDropped, "MQTT-MSG");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (!checkHeapHealth()) {
        stats.mqttDropped++;
        return;
    }

    String msg;
    msg.reserve(length + 1);
    msg.concat((char*)payload, length);

    safePush(mqttCallbackQueue, std::make_pair(String(topic), msg),
             mqttCallbackQueueMutex, stats.mqttDropped, "MQTT-CB");
}

void checkWiFiAndMQTT(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!WiFi.isConnected()) {
            WiFi.reconnect();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
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

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void checkPeers(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        unsigned long now = millis();

        if (xSemaphoreTake(peersMapMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (auto it = peers.begin(); it != peers.end();) {
                if (now - it->second.lastSeen > 60000) {
                    DEBUG_INFO("Removing stale peer: %u", it->first);
                    it = peers.erase(it);
                } else {
                    ++it;
                }
            }
            xSemaphoreGive(peersMapMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void statusReport(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        JsonDocument doc;
        doc["rssi"] = WiFi.RSSI();
        doc["uptime"] = millis() / 1000;
        doc["freeHeap"] = ESP.getFreeHeap();
        doc["deviceId"] = device_id;
        doc["parentId"] = device_id;
        doc["type"] = "root";
        doc["firmware"] = fw_md5;
        doc["clicks"] = stats.buttonPresses;
        doc["disconnects"] = 0;
        doc["lowHeap"] = stats.lowHeapEvents;

        if (xSemaphoreTake(peersMapMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            doc["peerCount"] = peers.size();
            xSemaphoreGive(peersMapMutex);
        }

        String msg;
        serializeJson(doc, msg);

        if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
            safePush(mqttMessageQueue,
                     std::make_pair(String("/switch/state/root"), msg),
                     mqttMessageQueueMutex, stats.mqttDropped, "MQTT-MSG");
        }

        vTaskDelay(pdMS_TO_TICKS(STATUS_REPORT_INTERVAL));
    }
}

void handleUpdateMessage(const String& topic) {
    DEBUG_INFO("Update requested: %s", topic.c_str());

    if (topic == "/switch/cmd/root") {
        otaInProgress = true;
        return;
    }

    String path = topic;
    int lastSlash = path.lastIndexOf('/');
    String last = path.substring(lastSlash + 1);
    uint64_t nodeId;

    if (toUint64(last, nodeId)) {
        safePush(espnowMessageQueue,
                 std::make_pair((uint32_t)nodeId, String("U")),
                 espnowMessageQueueMutex, stats.espnowDropped, "ESPNOW-MSG");
        return;
    }

    if (xSemaphoreTake(peersMapMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto& pair : peers) {
            uint32_t nodeId = pair.first;
            String nodeType = pair.second.nodeType;

            if ((nodeType == "relay" && topic == "/switch/cmd") ||
                (nodeType == "switch" && topic == "/relay/cmd")) {
                continue;
            }

            xSemaphoreGive(peersMapMutex);
            safePush(espnowMessageQueue, std::make_pair(nodeId, String("U")),
                     espnowMessageQueueMutex, stats.espnowDropped,
                     "ESPNOW-MSG");
            xSemaphoreTake(peersMapMutex, pdMS_TO_TICKS(100));
        }
        xSemaphoreGive(peersMapMutex);
    }
}

void sendMQTTMessages(void* pvParameters) {
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
        mqttClient.publish(message.first.c_str(), message.second.c_str());
    }
}

void sendESPNowMessages(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        std::pair<uint32_t, String> message;
        if (safePop(espnowPriorityQueue, message, espnowPriorityQueueMutex)) {
            sendESPNowMessage(message.first, message.second, true);
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (!safePop(espnowMessageQueue, message, espnowMessageQueueMutex)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
        sendESPNowMessage(message.first, message.second, false);
    }
}

void mqttCallbackTask(void* pvParameters) {
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
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

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

        bool isRelayCommand = topic.startsWith("/relay/cmd/");

        size_t lastSlash = topic.lastIndexOf('/');
        if (lastSlash != -1 && lastSlash < topic.length() - 1) {
            String idStr = topic.substring(lastSlash + 1);
            uint32_t nodeId = idStr.toInt();

            if (nodeId != 0 || idStr == "0") {
                if (xSemaphoreTake(peersMapMutex, pdMS_TO_TICKS(50)) !=
                    pdTRUE) {
                    continue;
                }

                bool found = (peers.count(nodeId) > 0);
                xSemaphoreGive(peersMapMutex);

                if (!found) {
                    continue;
                }

                DEBUG_INFO("MQTT → Node %u: %s", nodeId, msg.c_str());

                if (isRelayCommand) {
                    safePush(espnowPriorityQueue, std::make_pair(nodeId, msg),
                             espnowPriorityQueueMutex, stats.espnowDropped,
                             "ESPNOW-PRI");
                } else {
                    safePush(espnowMessageQueue, std::make_pair(nodeId, msg),
                             espnowMessageQueueMutex, stats.espnowDropped,
                             "ESPNOW-MSG");
                }
            }
        }
    }
}

void espnowCallbackTask(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        espnow_message_t message;
        if (!safePop(espnowCallbackQueue, message, espnowCallbackQueueMutex)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(5));

        uint32_t from = message.nodeId;
        String msg = String(message.data);

        // Update last seen
        if (xSemaphoreTake(peersMapMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            auto it = peers.find(from);
            if (it != peers.end()) {
                it->second.lastSeen = millis();
            }
            xSemaphoreGive(peersMapMutex);
        }

        if (msg == "R") {
            DEBUG_INFO("Node %u = relay", from);

            if (xSemaphoreTake(peersMapMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                auto it = peers.find(from);
                if (it != peers.end()) {
                    it->second.nodeType = "relay";
                }
                xSemaphoreGive(peersMapMutex);
            }

            safePush(espnowMessageQueue, std::make_pair(from, String("A")),
                     espnowMessageQueueMutex, stats.espnowDropped,
                     "ESPNOW-MSG");
            sendConnectionToNode(from);
            continue;
        }

        if (msg == "S") {
            DEBUG_INFO("Node %u = switch", from);

            if (xSemaphoreTake(peersMapMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                auto it = peers.find(from);
                if (it != peers.end()) {
                    it->second.nodeType = "switch";
                }
                xSemaphoreGive(peersMapMutex);
            }

            safePush(espnowMessageQueue, std::make_pair(from, String("A")),
                     espnowMessageQueueMutex, stats.espnowDropped,
                     "ESPNOW-MSG");
            sendConnectionToNode(from);
            continue;
        }

        // **BUTTON PRESS ROUTING** - This is the key part!
        if (msg.length() == 1 && msg[0] >= 'a' && msg[0] < 'a' + NLIGHTS) {
            char button = msg[0];
            stats.buttonPresses++;

            DEBUG_INFO("Button '%c' from switch %u", button, from);

            // Publish to MQTT
            String mqttTopic = "/switch/state/" + String(from);
            safePush(mqttMessageQueue, std::make_pair(mqttTopic, msg),
                     mqttMessageQueueMutex, stats.mqttDropped, "MQTT-MSG");

            // Route to target relays
            if (xSemaphoreTake(connectionsMapMutex, pdMS_TO_TICKS(100)) ==
                pdTRUE) {
                String fromIdStr = String(from);
                auto nodeIt = connections.find(fromIdStr);

                if (nodeIt != connections.end()) {
                    auto buttonIt = nodeIt->second.find(button);

                    if (buttonIt != nodeIt->second.end()) {
                        DEBUG_INFO("  Routing to %d targets:",
                                   buttonIt->second.size());

                        for (const auto& target : buttonIt->second) {
                            uint32_t targetNodeId = target.first.toInt();
                            String command = target.second;

                            DEBUG_INFO("    → %u: %s", targetNodeId,
                                       command.c_str());
                            stats.commandsRouted++;

                            xSemaphoreGive(connectionsMapMutex);
                            safePush(espnowPriorityQueue,
                                     std::make_pair(targetNodeId, command),
                                     espnowPriorityQueueMutex,
                                     stats.espnowDropped, "ESPNOW-PRI");
                            xSemaphoreTake(connectionsMapMutex,
                                           pdMS_TO_TICKS(100));
                        }
                    } else {
                        DEBUG_INFO("  No targets for button '%c'", button);
                    }
                } else {
                    DEBUG_INFO("  No config for switch %u", from);
                }

                xSemaphoreGive(connectionsMapMutex);
            }

            continue;
        }

        // Relay state confirmation (e.g., "A0", "B1")
        if (msg.length() == 2 && msg[0] >= 'A' && msg[0] < 'A' + NLIGHTS) {
            handleRelayMessage(from, msg);
            continue;
        }

        // JSON status reports
        if (isValidJson(msg)) {
            JsonDocument doc;
            deserializeJson(doc, msg.c_str());
            doc["parentId"] = device_id;

            String newMsg;
            serializeJson(doc, newMsg);

            if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
                safePush(mqttMessageQueue,
                         std::make_pair(String("/switch/state/root"), newMsg),
                         mqttMessageQueueMutex, stats.mqttDropped, "MQTT-MSG");
            }
            continue;
        }
    }
}

void broadcastDiscovery(void* pvParameters) {
    DEBUG_INFO("Discovery task started");

    uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastMac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
        DEBUG_INFO("Broadcast peer added");
    }

    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        espnow_message_t msg;
        msg.nodeId = device_id;
        msg.msgType = 'Q';
        strcpy(msg.data, "Q");

        esp_now_send(broadcastMac, (uint8_t*)&msg, sizeof(espnow_message_t));

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void setup() {
    Serial.begin(115200);
    vTaskDelay(500);

    fw_md5 = ESP.getSketchMD5();
    DEBUG_INFO("=== ESP-NOW Root Starting ===");
    DEBUG_INFO("Firmware: %s", fw_md5.c_str());
    DEBUG_INFO("Free heap: %u", ESP.getFreeHeap());

    DEBUG_INFO("Creating mutexes...");
    mqttMessageQueueMutex = xSemaphoreCreateMutex();
    mqttCallbackQueueMutex = xSemaphoreCreateMutex();
    espnowMessageQueueMutex = xSemaphoreCreateMutex();
    espnowPriorityQueueMutex = xSemaphoreCreateMutex();
    espnowCallbackQueueMutex = xSemaphoreCreateMutex();
    peersMapMutex = xSemaphoreCreateMutex();
    connectionsMapMutex = xSemaphoreCreateMutex();

    if (!mqttMessageQueueMutex || !mqttCallbackQueueMutex ||
        !espnowMessageQueueMutex || !espnowPriorityQueueMutex ||
        !espnowCallbackQueueMutex || !peersMapMutex || !connectionsMapMutex) {
        DEBUG_ERROR("FATAL: Mutex creation failed!");
        ESP.restart();
    }

    espnowInit();

    DEBUG_INFO("Creating tasks...");
    xTaskCreatePinnedToCore(checkWiFiAndMQTT, "WiFiMQTT", 8192, NULL, 2, NULL,
                            1);
    xTaskCreatePinnedToCore(statusReport, "Status", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(checkPeers, "Peers", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(sendMQTTMessages, "MQTT_TX", 4096, NULL, 2, NULL,
                            1);
    xTaskCreatePinnedToCore(sendESPNowMessages, "ESPNOW_TX", 4096, NULL, 3,
                            NULL, 0);
    xTaskCreatePinnedToCore(mqttCallbackTask, "MQTT_CB", 12288, NULL, 4, NULL,
                            1);
    xTaskCreatePinnedToCore(espnowCallbackTask, "ESPNOW_CB", 12288, NULL, 4,
                            NULL, 0);
    xTaskCreatePinnedToCore(broadcastDiscovery, "Discovery", 4096, NULL, 1,
                            NULL, 1);

    DEBUG_INFO("=== Init complete ===");
}

void loop() {
    static bool otaTaskStarted = false;

    if (otaInProgress && !otaTaskStarted) {
        otaTaskStarted = true;
        DEBUG_INFO("Starting OTA");
        xTaskCreatePinnedToCore(otaTask, "OTA", 16384, NULL, 5, NULL, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(otaInProgress ? 1000 : 10));
}