#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <credentials.h>
#include <painlessMesh.h>

#include <queue>

#include "esp_task_wdt.h"

// Hardware definitions
#define USE_BOARD 75
#define NO_PWMPIN
const byte wifiActivityPin = 255;
#define NLIGHTS 8

// Timing constants
#define STATUS_PRINT_INTERVAL 10000
#define WIFI_CONNECT_TIMEOUT 20000
#define REGISTRATION_RETRY_INTERVAL 10000
#define STATUS_REPORT_INTERVAL 15000
#define BUTTON_DEBOUNCE_TIME 1000
#define RESET_TIMEOUT 300000
#define OTA_START_DELAY 5000

// Queue size limits
#define MAX_QUEUE_SIZE 40
#define CRITICAL_HEAP_THRESHOLD 25000
#define LOW_HEAP_THRESHOLD 50000

// Minimal debug - only errors and critical events
#define DEBUG_LEVEL 3  // 0=none, 1=errors only, 2=info, 3=verbose

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

// Function declarations
void receivedCallback(const uint32_t& from, const String& msg);
void meshInit();
void onNewConnection(uint32_t nodeId);
void onDroppedConnection(uint32_t nodeId);
void performFirmwareUpdate();

painlessMesh mesh;

uint32_t rootId = 0;
uint32_t deviceId = 0;
uint32_t disconnects = 0;
uint32_t clicks = 0;
String fw_md5;

const int relays[NLIGHTS] = {32, 33, 25, 26, 27, 14, 12, 13};
const int buttons[NLIGHTS] = {2, 15, 4, 0, 17, 16, 18, 5};
int lights[NLIGHTS] = {0, 0, 0, 0, 0, 0, 0, 0};
volatile bool buttonState[NLIGHTS] = {0, 0, 0, 0, 0, 0, 0, 0};
volatile uint32_t lastPress[NLIGHTS] = {0, 0, 0, 0, 0, 0, 0, 0};
volatile uint8_t pressed = 0;

// Queues
std::queue<std::pair<uint32_t, String>> meshCallbackQueue;
std::queue<std::pair<uint32_t, String>> meshMessageQueue;
std::queue<std::pair<uint32_t, String>> meshPriorityQueue;

// Mutexes for thread safety
SemaphoreHandle_t meshCallbackQueueMutex = NULL;
SemaphoreHandle_t meshMessageQueueMutex = NULL;
SemaphoreHandle_t meshPriorityQueueMutex = NULL;
SemaphoreHandle_t lightsArrayMutex = NULL;
SemaphoreHandle_t myConnectionsMutex = NULL;

// Statistics
struct Statistics {
    uint32_t meshDropped = 0;
    uint32_t lowHeapEvents = 0;
    uint32_t criticalHeapEvents = 0;
} stats;

// State variables
bool registeredWithRoot = false;
uint32_t resetTimer = 0;
uint32_t otaTimer = 0;
bool otaTimerStarted = false;
volatile bool otaInProgress = false;

std::map<char, std::vector<std::pair<String, String>>> myConnections;

Preferences preferences;
String connectionsHash = "";

// Helper function to safely push to bounded queue
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

// Helper function to safely pop from queue
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

// Check heap and handle critical memory situations
bool checkHeapHealth() {
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < CRITICAL_HEAP_THRESHOLD) {
        stats.criticalHeapEvents++;
        DEBUG_ERROR("CRITICAL: Low heap %u bytes! Clearing queues...",
                    freeHeap);
        if (xSemaphoreTake(meshMessageQueueMutex, pdMS_TO_TICKS(100)) ==
            pdTRUE) {
            while (!meshMessageQueue.empty()) meshMessageQueue.pop();
            xSemaphoreGive(meshMessageQueueMutex);
        }
        return false;
    } else if (freeHeap < LOW_HEAP_THRESHOLD) {
        stats.lowHeapEvents++;
        DEBUG_ERROR("Low heap: %u bytes", freeHeap);
        return false;
    }
    return true;
}

void otaTask(void* pv) {
    for (int i = 0; i < NLIGHTS; i++) {
        detachInterrupt(buttons[i]);
    }
    DEBUG_INFO("[OTA] Stopping mesh...");
    mesh.stop();
    esp_task_wdt_deinit();
    vTaskDelay(pdMS_TO_TICKS(2000));
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
        DEBUG_INFO("[OTA] Starting update attempt %d/%d...", attemptCount,
                   MAX_RETRIES);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - startTime > WIFI_CONNECT_TIMEOUT) {
                DEBUG_ERROR("[OTA] WiFi timeout on attempt %d", attemptCount);
                break;
            }
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
        if (WiFi.status() != WL_CONNECTED) {
            if (attemptCount < MAX_RETRIES) {
                DEBUG_INFO("[OTA] Retrying in 2 seconds...");
                vTaskDelay(2000 / portTICK_PERIOD_MS);
                continue;
            } else {
                DEBUG_ERROR("[OTA] All WiFi connection attempts failed");
                break;
            }
        }
        WiFiClientSecure* client = new WiFiClientSecure();
        if (!client) {
            DEBUG_ERROR("[OTA] Client alloc failed on attempt %d",
                        attemptCount);
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
            DEBUG_ERROR("[OTA] HTTP begin failed on attempt %d", attemptCount);
            delete client;
            if (attemptCount < MAX_RETRIES) {
                vTaskDelay(2000 / portTICK_PERIOD_MS);
                continue;
            }
            break;
        }
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            int contentLength = http.getSize();
            if (contentLength <= 0 || !Update.begin(contentLength)) {
                DEBUG_ERROR("[OTA] Invalid size or begin failed on attempt %d",
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
            if (written != contentLength) {
                DEBUG_ERROR("[OTA] Write mismatch on attempt %d", attemptCount);
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
                DEBUG_INFO("[OTA] Update successful on attempt %d!",
                           attemptCount);
                updateSuccess = true;
                http.end();
                delete client;
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                ESP.restart();
                return;
            } else {
                DEBUG_ERROR("[OTA] Update.end() failed on attempt %d",
                            attemptCount);
                http.end();
                delete client;
                if (attemptCount < MAX_RETRIES) {
                    vTaskDelay(2000 / portTICK_PERIOD_MS);
                    continue;
                }
            }
        } else {
            DEBUG_ERROR("[OTA] HTTP failed with code %d on attempt %d",
                        httpCode, attemptCount);
            http.end();
            delete client;
            if (attemptCount < MAX_RETRIES) {
                vTaskDelay(2000 / portTICK_PERIOD_MS);
                continue;
            }
        }
    }
    DEBUG_ERROR("[OTA] All %d update attempts failed. Restarting...",
                MAX_RETRIES);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP.restart();
}

void meshInit() {
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA);
    mesh.onReceive(&receivedCallback);
    mesh.onNewConnection(&onNewConnection);
    mesh.onDroppedConnection(&onDroppedConnection);
    deviceId = mesh.getNodeId();
    DEBUG_INFO("RELAY: Device ID: %u", deviceId);
    DEBUG_VERBOSE("RELAY: Free heap: %d bytes", ESP.getFreeHeap());
}

void sendStatusReport(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        DEBUG_VERBOSE("MESH: Sending status report to root");
        JsonDocument doc;
        doc["rssi"] = WiFi.RSSI();
        doc["uptime"] = millis() / 1000;
        doc["clicks"] = clicks;
        doc["disconnects"] = disconnects;
        doc["parentId"] = 0;
        doc["deviceId"] = deviceId;
        doc["freeHeap"] = ESP.getFreeHeap();
        doc["type"] = "relay";
        doc["firmware"] = fw_md5;
        doc["meshDropped"] = stats.meshDropped;
        doc["lowHeap"] = stats.lowHeapEvents;
        doc["criticalHeap"] = stats.criticalHeapEvents;
        String msg;
        serializeJson(doc, msg);
        safePush(meshMessageQueue, std::make_pair(rootId, msg),
                 meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
        vTaskDelay(pdMS_TO_TICKS(STATUS_REPORT_INTERVAL));
    }
}

void syncLightStates() {
    DEBUG_INFO("RELAY: Syncing all light states to root");
    if (xSemaphoreTake(lightsArrayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < NLIGHTS; i++) {
            char message[3];
            message[0] = 'A' + i;
            message[1] = lights[i] ? '1' : '0';
            message[2] = '\0';
            safePush(meshMessageQueue, std::make_pair(rootId, String(message)),
                     meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
        }
        xSemaphoreGive(lightsArrayMutex);
    }
}

void statusPrintTask(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        DEBUG_INFO("\n--- Status Report ---");
        DEBUG_INFO("Device ID: %u", deviceId);
        DEBUG_INFO("Root ID: %u", rootId);
        DEBUG_INFO("Registered: %s", registeredWithRoot ? "Yes" : "No");
        DEBUG_INFO("Free Heap: %d bytes", ESP.getFreeHeap());
        DEBUG_INFO("Uptime: %lu seconds", millis() / 1000);
        DEBUG_INFO("Sketch MD5: %s", fw_md5.c_str());
        DEBUG_INFO("Dropped messages: %u", stats.meshDropped);
        if (xSemaphoreTake(lightsArrayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            DEBUG_VERBOSE("\nRelay States:");
            for (int i = 0; i < NLIGHTS; i++) {
                DEBUG_VERBOSE("  Light %c (Pin %d): %s", 'a' + i, relays[i],
                              lights[i] ? "ON" : "OFF");
            }
            xSemaphoreGive(lightsArrayMutex);
        }
        auto nodes = mesh.getNodeList();
        DEBUG_INFO("\nMesh Network: %u node(s)", nodes.size());
        for (auto node : nodes) {
            DEBUG_VERBOSE("  Node: %u%s", node,
                          (node == rootId) ? " (ROOT)" : "");
        }
        DEBUG_INFO("-------------------\n");
        vTaskDelay(pdMS_TO_TICKS(STATUS_PRINT_INTERVAL));
    }
}

void resetTask(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (mesh.getNodeList().empty()) {
            registeredWithRoot = false;
        } else if (registeredWithRoot) {
            resetTimer = millis();
        }

        if (!otaTimerStarted)
            otaTimer = millis();
        else if ((millis() - otaTimer) > OTA_START_DELAY) {
            otaInProgress = true;
        }

        if ((millis() - resetTimer) > RESET_TIMEOUT) ESP.restart();

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

String calculateConnectionsHash(const String& jsonStr) {
    // Simple hash using sum of characters (you can use MD5 for better hash)
    unsigned long hash = 0;
    for (unsigned int i = 0; i < jsonStr.length(); i++) {
        hash = hash * 31 + jsonStr[i];
    }
    return String(hash, HEX);
}

bool saveConnectionsToNVS(const String& jsonStr) {
    if (!preferences.begin("connections", false)) {
        DEBUG_ERROR("saveConnectionsToNVS: Failed to open NVS");
        return false;
    }

    // Check size limit (NVS has max ~4000 bytes per key)
    if (jsonStr.length() > 4000) {
        DEBUG_ERROR("saveConnectionsToNVS: JSON too large (%d bytes)",
                    jsonStr.length());
        preferences.end();
        return false;
    }

    // Save the JSON string
    size_t written = preferences.putString("config", jsonStr);
    if (written == 0) {
        DEBUG_ERROR("saveConnectionsToNVS: Failed to write config");
        preferences.end();
        return false;
    }

    // Save hash for quick comparison
    String hash = calculateConnectionsHash(jsonStr);
    preferences.putString("hash", hash);
    connectionsHash = hash;

    preferences.end();

    DEBUG_INFO("saveConnectionsToNVS: Saved %d bytes, hash: %s", written,
               hash.c_str());
    return true;
}

String loadConnectionsFromNVS() {
    if (!preferences.begin("connections", true)) {  // true = read-only
        DEBUG_ERROR("loadConnectionsFromNVS: Failed to open NVS");
        return "";
    }

    String jsonStr = preferences.getString("config", "");
    String savedHash = preferences.getString("hash", "");

    preferences.end();

    if (jsonStr.length() == 0) {
        DEBUG_INFO("loadConnectionsFromNVS: No saved connections found");
        return "";
    }

    // Verify hash
    String calculatedHash = calculateConnectionsHash(jsonStr);
    if (savedHash != calculatedHash) {
        DEBUG_ERROR(
            "loadConnectionsFromNVS: Hash mismatch! Data may be corrupted");
        DEBUG_ERROR("  Saved: %s, Calculated: %s", savedHash.c_str(),
                    calculatedHash.c_str());
        return "";
    }

    connectionsHash = savedHash;
    DEBUG_INFO("loadConnectionsFromNVS: Loaded %d bytes, hash: %s",
               jsonStr.length(), savedHash.c_str());
    DEBUG_INFO("loadConnectionsFromNVS: Connections data: %s", jsonStr.c_str());

    return jsonStr;
}

void clearConnectionsFromNVS() {
    if (!preferences.begin("connections", false)) {
        DEBUG_ERROR("clearConnectionsFromNVS: Failed to open NVS");
        return;
    }

    preferences.clear();
    preferences.end();
    connectionsHash = "";

    DEBUG_INFO("clearConnectionsFromNVS: Cleared all saved connections");
}

bool hasConnectionsChanged(const String& newJsonStr) {
    String newHash = calculateConnectionsHash(newJsonStr);
    bool changed = (newHash != connectionsHash);

    if (changed) {
        DEBUG_INFO("hasConnectionsChanged: YES (old: %s, new: %s)",
                   connectionsHash.c_str(), newHash.c_str());
    } else {
        DEBUG_VERBOSE("hasConnectionsChanged: NO (hash: %s)", newHash.c_str());
    }

    return changed;
}

void processConnectionsJSON(const String& jsonStr) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, jsonStr);

    if (err) {
        DEBUG_ERROR("receiveConnections: Failed to parse JSON: %s",
                    err.c_str());
        return;
    }

    if (xSemaphoreTake(myConnectionsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        DEBUG_ERROR("receiveConnections: Failed to acquire mutex");
        return;
    }

    // Clear existing connections
    myConnections.clear();

    String myIdStr = String(deviceId);

    // Check if our device ID exists in the JSON
    if (doc[myIdStr].isNull()) {
        xSemaphoreGive(myConnectionsMutex);
        DEBUG_INFO(
            "receiveConnections: No connections configured for this device");

        // Save empty config
        saveConnectionsToNVS("{}");
        return;
    }

    JsonObject myConfig = doc[myIdStr].as<JsonObject>();

    // Parse each letter (button) configuration
    int totalTargets = 0;
    for (JsonPair letterPair : myConfig) {
        char letter = letterPair.key().c_str()[0];
        JsonArray targetsArray = letterPair.value().as<JsonArray>();

        std::vector<std::pair<String, String>> targets;
        targets.reserve(targetsArray.size());

        for (JsonArray targetPair : targetsArray) {
            if (targetPair.size() >= 2) {
                String targetId = targetPair[0].as<String>();
                String command = targetPair[1].as<String>();
                targets.emplace_back(targetId, command);
                totalTargets++;

                DEBUG_VERBOSE("  Button '%c' -> Node %s: %s", letter,
                              targetId.c_str(), command.c_str());
            }
        }

        myConnections[letter] = std::move(targets);
    }

    xSemaphoreGive(myConnectionsMutex);

    DEBUG_INFO("receiveConnections: Loaded %d buttons, %d total targets",
               myConnections.size(), totalTargets);
}

void receiveConnections(const String& jsonStr) {
    if (!myConnectionsMutex) {
        DEBUG_ERROR("receiveConnections: Mutex not initialized!");
        return;
    }

    processConnectionsJSON(jsonStr);

    // Check if connections actually changed
    if (!hasConnectionsChanged(jsonStr)) {
        DEBUG_INFO("receiveConnections: No changes detected, skipping update");
        return;
    }

    // Save to NVS
    if (saveConnectionsToNVS(jsonStr)) {
        DEBUG_INFO("receiveConnections: Saved to NVS successfully");
    } else {
        DEBUG_ERROR("receiveConnections: Failed to save to NVS");
    }
}

void loadConnectionsOnBoot() {
    DEBUG_INFO("loadConnectionsOnBoot: Loading saved connections...");

    String savedJson = loadConnectionsFromNVS();

    if (savedJson.length() == 0) {
        DEBUG_INFO(
            "loadConnectionsOnBoot: No saved connections, will wait for config "
            "from root");
        return;
    }

    // Parse and apply saved connections
    receiveConnections(savedJson);

    DEBUG_INFO("loadConnectionsOnBoot: Restored connections from NVS");
}

void printConnectionsStats() {
    if (!preferences.begin("connections", true)) {
        DEBUG_ERROR("printConnectionsStats: Failed to open NVS");
        return;
    }

    String config = preferences.getString("config", "");
    String hash = preferences.getString("hash", "");

    preferences.end();

    DEBUG_INFO("\n--- Connections NVS Stats ---");
    DEBUG_INFO("Stored size: %d bytes", config.length());
    DEBUG_INFO("Stored hash: %s", hash.c_str());
    DEBUG_INFO("Current hash: %s", connectionsHash.c_str());
    DEBUG_INFO("Max NVS size: 4000 bytes");
    DEBUG_INFO("Available: %d bytes", 4000 - config.length());

    if (xSemaphoreTake(myConnectionsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        DEBUG_INFO("Active buttons: %d", myConnections.size());
        int totalTargets = 0;
        for (const auto& conn : myConnections) {
            totalTargets += conn.second.size();
        }
        DEBUG_INFO("Total targets: %d", totalTargets);
        xSemaphoreGive(myConnectionsMutex);
    }
    DEBUG_INFO("---------------------------\n");
}

String exportConnections() {
    if (xSemaphoreTake(myConnectionsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        DEBUG_ERROR("exportConnections: Failed to acquire mutex");
        return "";
    }

    JsonDocument doc;
    String myIdStr = String(deviceId);
    JsonObject myConfig = doc[myIdStr].to<JsonObject>();

    for (const auto& letterPair : myConnections) {
        char letter = letterPair.first;
        JsonArray targetsArray = myConfig[String(letter)].to<JsonArray>();

        for (const auto& target : letterPair.second) {
            JsonArray targetPair = targetsArray.add<JsonArray>();
            targetPair.add(target.first);
            targetPair.add(target.second);
        }
    }

    xSemaphoreGive(myConnectionsMutex);

    String jsonStr;
    serializeJson(doc, jsonStr);
    return jsonStr;
}

std::vector<std::pair<String, String>> getTargetsForButton(char button) {
    std::vector<std::pair<String, String>> result;

    if (!myConnectionsMutex) return result;

    if (xSemaphoreTake(myConnectionsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        auto it = myConnections.find(button);
        if (it != myConnections.end()) {
            result = it->second;
        }
        xSemaphoreGive(myConnectionsMutex);
    }

    return result;
}

void buttonPressTask(void* pvParameters) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));

        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!pressed) continue;

        for (int i = 0; i < NLIGHTS; i++) {
            if (pressed & (1 << i)) {
                pressed &= ~(1 << i);
            } else {
                continue;
            }

            char button = 'a' + i;
            DEBUG_VERBOSE("BUTTON: Button %d pressed ('%c')", i, button);

            auto targets = getTargetsForButton(button);

            if (targets.empty()) {
                DEBUG_INFO("RELAY: No targets configured for button %c",
                           button);
                continue;
            }

            DEBUG_INFO("BUTTON: Sending to %d targets", targets.size());

            for (const auto& target : targets) {
                uint32_t targetId = target.first.toInt();
                String command = target.second;

                DEBUG_VERBOSE("  -> Node %u: %s", targetId, command.c_str());

                if (targetId == deviceId) {
                    uint32_t lightIndex = command[0] - 'a';
                    if (xSemaphoreTake(lightsArrayMutex, pdMS_TO_TICKS(50)) ==
                        pdTRUE) {
                        lights[lightIndex] = (lights[lightIndex]) ? 0 : 1;
                        digitalWrite(relays[lightIndex],
                                     lights[lightIndex] ? HIGH : LOW);
                        clicks++;
                        DEBUG_INFO(
                            "RELAY: Light %c set to %s by local button press",
                            button, lights[lightIndex] ? "ON" : "OFF");
                        xSemaphoreGive(lightsArrayMutex);
                    }
                } else {
                    safePush(meshPriorityQueue,
                             std::make_pair(targetId, command),
                             meshPriorityQueueMutex, stats.meshDropped,
                             "MESH-PRIORITY");
                }

                safePush(meshMessageQueue, std::make_pair(rootId, command),
                         meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
            }
        }
    }
}

void registerTask(void* pvParameters) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(REGISTRATION_RETRY_INTERVAL));
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!registeredWithRoot) {
            DEBUG_INFO("MESH: Attempting registration with root...");
            digitalWrite(23, LOW);
            if (rootId == 0) {
                DEBUG_ERROR("MESH: Root ID unknown, cannot register");
                continue;
            }
            safePush(meshMessageQueue, std::make_pair(rootId, String("R")),
                     meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
            DEBUG_VERBOSE("MESH: Sent registration 'R' to root %u", rootId);
        } else {
            digitalWrite(23, HIGH);
        }
    }
}

void IRAM_ATTR buttonISR(void* arg) {
    int index = (intptr_t)arg;
    uint32_t now = micros();
    if (now - lastPress[index] > BUTTON_DEBOUNCE_TIME * 1000) {
        lastPress[index] = now;
        buttonState[index] = !digitalRead(buttons[index]);
        pressed |= (1 << index);
    }
}

void onDroppedConnection(uint32_t nodeId) {
    DEBUG_INFO("MESH: Lost connection to node %u", nodeId);
    if (nodeId == rootId) {
        DEBUG_ERROR("MESH: Lost connection to root, resetting");
        disconnects++;
        registeredWithRoot = false;
    }
}

void onNewConnection(uint32_t nodeId) {
    DEBUG_INFO("MESH: New connection from node %u", nodeId);
    if (rootId == 0) {
        DEBUG_ERROR("MESH: Root ID unknown, cannot register");
        return;
    }
    safePush(meshMessageQueue, std::make_pair(rootId, String("R")),
             meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
    DEBUG_VERBOSE("MESH: Sent registration 'R' to root %u", rootId);
}

void receivedCallback(const uint32_t& from, const String& msg) {
    if (!checkHeapHealth()) {
        stats.meshDropped++;
        return;
    }
    DEBUG_VERBOSE("MESH: [%u] %s", from, msg.c_str());
    safePush(meshCallbackQueue, std::make_pair(from, msg),
             meshCallbackQueueMutex, stats.meshDropped, "MESH-CB");
}

void processMeshMessage(const uint32_t& from, const String& msg) {
    if (msg.startsWith("{")) {
        DEBUG_INFO("MESH: Received connections configuration from %u", from);
        receiveConnections(msg);
        return;
    }

    if (msg == "S") {
        DEBUG_INFO("MESH: Root %u requesting state sync", from);
        syncLightStates();
        return;
    }

    if (msg == "Q") {
        DEBUG_INFO("MESH: Registration query received from root");
        rootId = from;
        safePush(meshMessageQueue, std::make_pair(rootId, String("R")),
                 meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
        DEBUG_VERBOSE("MESH: Sent registration 'R' to root %u", rootId);
        return;
    }

    if (msg == "U") {
        DEBUG_INFO("MESH: Firmware update command received");
        otaTimerStarted = true;
        return;
    }

    // Handle light control with state (e.g., "a0", "b1")
    if (msg.length() == 2 && msg[0] >= 'a' && msg[0] < 'a' + NLIGHTS) {
        int lightIndex = msg[0] - 'a';
        int newState = msg[1] - '0';
        if (newState != 0 && newState != 1) {
            DEBUG_ERROR("MESH: Invalid state '%c' in message from %u", msg[1],
                        rootId);
            return;
        }
        if (xSemaphoreTake(lightsArrayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            lights[lightIndex] = newState;
            digitalWrite(relays[lightIndex], newState ? HIGH : LOW);
            clicks++;
            DEBUG_INFO("RELAY: Light %c set to %s by root %u", 'a' + lightIndex,
                       newState ? "ON" : "OFF", rootId);

            // Send confirmation back via priority queue for fast response
            char response[3];
            response[0] = 'A' + lightIndex;
            response[1] = newState ? '1' : '0';
            response[2] = '\0';
            xSemaphoreGive(lightsArrayMutex);
            safePush(
                meshPriorityQueue, std::make_pair(rootId, String(response)),
                meshPriorityQueueMutex, stats.meshDropped, "MESH-PRIORITY");
        }
        return;
    }

    // Handle light toggle (e.g., "a", "b")
    if (msg.length() == 1 && msg[0] >= 'a' && msg[0] < 'a' + NLIGHTS) {
        int lightIndex = msg[0] - 'a';
        if (xSemaphoreTake(lightsArrayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            lights[lightIndex] = !lights[lightIndex];
            digitalWrite(relays[lightIndex], lights[lightIndex] ? HIGH : LOW);
            clicks++;
            DEBUG_INFO("RELAY: Light %c toggled to %s by root %u",
                       'a' + lightIndex, lights[lightIndex] ? "ON" : "OFF",
                       rootId);

            // Send confirmation back via priority queue for fast response
            char response[3];
            response[0] = 'A' + lightIndex;
            response[1] = lights[lightIndex] ? '1' : '0';
            response[2] = '\0';
            xSemaphoreGive(lightsArrayMutex);
            safePush(
                meshPriorityQueue, std::make_pair(rootId, String(response)),
                meshPriorityQueueMutex, stats.meshDropped, "MESH-PRIORITY");
        }
        return;
    }

    if (msg == "A") {
        DEBUG_INFO("MESH: Registration accepted by root");
        registeredWithRoot = true;
        return;
    }

    DEBUG_ERROR("MESH: Unknown/unhandled message '%s' from %u", msg.c_str(),
                from);
}

void meshCallbackTask(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        std::pair<uint32_t, String> message;
        if (!safePop(meshCallbackQueue, message, meshCallbackQueueMutex)) {
            vTaskDelay(pdMS_TO_TICKS(5));  // Reduced from 20ms
            continue;
        }
        // No artificial delay - process immediately
        uint32_t from = message.first;
        String msg = message.second;
        processMeshMessage(from, msg);
    }
}

void sendMeshMessages(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Check priority queue FIRST for relay state confirmations (instant
        // response)
        std::pair<uint32_t, String> message;
        if (safePop(meshPriorityQueue, message, meshPriorityQueueMutex)) {
            uint32_t to = message.first;
            String msg = message.second;
            mesh.sendSingle(to, msg);
            DEBUG_VERBOSE("MESH TX PRIORITY: [%u] %s", to, msg.c_str());
            vTaskDelay(pdMS_TO_TICKS(2));  // Minimal delay
            continue;
        }

        // Then check regular queue
        if (!safePop(meshMessageQueue, message, meshMessageQueueMutex)) {
            vTaskDelay(pdMS_TO_TICKS(5));  // Reduced from 20ms
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(5));  // Reduced rate limiting
        uint32_t to = message.first;
        String msg = message.second;
        mesh.sendSingle(to, msg);
        DEBUG_VERBOSE("MESH TX: [%u] %s", to, msg.c_str());
    }
}

void setup() {
    Serial.begin(115200);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    fw_md5 = ESP.getSketchMD5();
    DEBUG_INFO("\n\n========================================");
    DEBUG_INFO("ESP32 Mesh Relay Node Starting...");
    DEBUG_INFO("Chip Model: %s", ESP.getChipModel());
    DEBUG_INFO("Sketch MD5: %s", fw_md5.c_str());
    DEBUG_INFO("Chip Revision: %d", ESP.getChipRevision());
    DEBUG_INFO("CPU Frequency: %d MHz", ESP.getCpuFreqMHz());
    DEBUG_INFO("Free Heap: %d bytes", ESP.getFreeHeap());
    DEBUG_INFO("Flash Size: %d bytes", ESP.getFlashChipSize());
    DEBUG_INFO("========================================\n");

    // CRITICAL: Create mutexes BEFORE meshInit()
    DEBUG_INFO("Creating mutexes...");
    meshCallbackQueueMutex = xSemaphoreCreateMutex();
    meshMessageQueueMutex = xSemaphoreCreateMutex();
    meshPriorityQueueMutex = xSemaphoreCreateMutex();
    lightsArrayMutex = xSemaphoreCreateMutex();
    myConnectionsMutex = xSemaphoreCreateMutex();

    if (!meshCallbackQueueMutex || !meshMessageQueueMutex ||
        !meshPriorityQueueMutex || !lightsArrayMutex || !myConnectionsMutex) {
        DEBUG_ERROR("FATAL: Failed to create mutexes!");
        ESP.restart();
    }
    DEBUG_INFO("All mutexes created successfully");

    loadConnectionsOnBoot();

    // Initialize relay pins
    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(relays[i], OUTPUT);
        digitalWrite(relays[i], LOW);
        DEBUG_VERBOSE("RELAY: Initialized relay %d (Pin %d)", i, relays[i]);
    }

    pinMode(23, OUTPUT);
    digitalWrite(23, LOW);

    // Initialize mesh
    meshInit();

    // Initialize button interrupts
    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(buttons[i], INPUT_PULLDOWN);
        attachInterruptArg(digitalPinToInterrupt(buttons[i]), buttonISR,
                           (void*)i, CHANGE);
    }

    // Create tasks with optimized priorities
    DEBUG_INFO("Creating tasks...");
    xTaskCreatePinnedToCore(statusPrintTask, "StatusPrint", 4096, NULL, 1, NULL,
                            1);
    xTaskCreatePinnedToCore(sendStatusReport, "StatusReport", 8192, NULL, 1,
                            NULL, 1);
    xTaskCreatePinnedToCore(registerTask, "Register", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(buttonPressTask, "ButtonPress", 4096, NULL, 2, NULL,
                            1);
    xTaskCreatePinnedToCore(resetTask, "Reset", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(meshCallbackTask, "MeshCallbackTask", 8192, NULL, 4,
                            NULL, 0);
    xTaskCreatePinnedToCore(sendMeshMessages, "SendMeshMessages", 8192, NULL, 3,
                            NULL, 0);  // Higher priority

    DEBUG_INFO("RELAY: Setup complete, waiting for mesh connections...");
}

void loop() {
    static bool otaTaskStarted = false;

    if (otaInProgress && !otaTaskStarted) {
        otaTaskStarted = true;
        DEBUG_INFO("[OTA] Disconnecting mesh...");
        xTaskCreatePinnedToCore(otaTask, "OTA", 8192, NULL, 5, NULL,
                                0);  // Increased stack size
    }
    if (otaInProgress) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
        mesh.update();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}