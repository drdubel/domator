#include <Adafruit_NeoPixel.h>
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
#include "esp_wifi.h"

// Pin and hardware definitions
#define NLIGHTS 7
#define LED_PIN 8
#define NUM_LEDS 1

// Timing constants
#define BUTTON_DEBOUNCE_TIME 250
#define STATUS_PRINT_INTERVAL 10000
#define WIFI_CONNECT_TIMEOUT 20000
#define REGISTRATION_RETRY_INTERVAL 10000
#define STATUS_REPORT_INTERVAL 15000
#define RESET_TIMEOUT 120000
#define OTA_START_DELAY 5000

// Queue size limits
#define MAX_QUEUE_SIZE 30
#define CRITICAL_HEAP_THRESHOLD 20000
#define LOW_HEAP_THRESHOLD 40000

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

// Function declarations
void receivedCallback(const uint32_t& from, const String& msg);
void onDroppedConnection(uint32_t nodeId);
void onNewConnection(uint32_t nodeId);
void meshInit();
void performFirmwareUpdate();
void setLedColor(uint8_t r, uint8_t g, uint8_t b);
void printNodes();

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
painlessMesh mesh;

uint32_t rootId = 0;
uint32_t deviceId = 0;
uint32_t disconnects = 0;
uint32_t clicks = 0;
String fw_md5;

// Queues
std::queue<std::pair<uint32_t, String>> meshCallbackQueue;
std::queue<std::pair<uint32_t, String>> meshMessageQueue;
std::queue<std::pair<uint32_t, String>>
    meshPriorityQueue;  // Fast path for button presses

// Mutexes for thread safety
SemaphoreHandle_t meshCallbackQueueMutex = NULL;
SemaphoreHandle_t meshMessageQueueMutex = NULL;
SemaphoreHandle_t meshPriorityQueueMutex = NULL;
SemaphoreHandle_t myConnectionsMutex = NULL;

// Statistics
struct Statistics {
    uint32_t meshDropped = 0;
    uint32_t lowHeapEvents = 0;
    uint32_t criticalHeapEvents = 0;
} stats;

std::map<char, std::vector<std::pair<String, String>>> myConnections;

Preferences preferences;
String connectionsHash = "";

const int buttonPins[NLIGHTS] = {A0, A1, A3, A4, A5, 6, 7};
uint32_t lastTimeClick[NLIGHTS] = {0};
int lastButtonState[NLIGHTS] = {HIGH};

// State variables
bool registeredWithRoot = false;
uint32_t resetTimer = 0;
uint32_t otaTimer = 0;
bool otaTimerStarted = false;
volatile bool otaInProgress = false;

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

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
    pixels.setPixelColor(0, pixels.Color(r, g, b));
    pixels.show();
}

void updateLedStatus(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        bool meshConnected = mesh.getNodeList().size() > 0;
        if (meshConnected && registeredWithRoot) {
            setLedColor(0, 255, 0);  // Green - fully connected
        } else if (meshConnected) {
            setLedColor(
                255, 255,
                0);  // Yellow - mesh connected, waiting for registration
        } else {
            setLedColor(255, 0, 0);  // Red - not connected
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void otaTask(void* pv) {
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
    mesh.setDebugMsgTypes(ERROR | STARTUP);
    esp_wifi_set_ps(WIFI_PS_NONE);
    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA);
    mesh.onReceive(&receivedCallback);
    mesh.onDroppedConnection(&onDroppedConnection);
    mesh.onNewConnection(&onNewConnection);
    deviceId = mesh.getNodeId();
    DEBUG_INFO("SWITCH: Device ID: %u", deviceId);
    DEBUG_INFO("SWITCH: Free heap: %d bytes", ESP.getFreeHeap());
}

void sendStatusReport(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (rootId == 0) {
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
        doc["type"] = "switch";
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

void receivedCallback(const uint32_t& from, const String& msg) {
    if (!checkHeapHealth()) {
        stats.meshDropped++;
        return;
    }
    DEBUG_VERBOSE("MESH: [%u] %s", from, msg.c_str());
    safePush(meshCallbackQueue, std::make_pair(from, msg),
             meshCallbackQueueMutex, stats.meshDropped, "MESH-CB");
}

void printNodes() {
    auto nodes = mesh.getNodeList();
    DEBUG_INFO("MESH: Connected to %u node(s)", nodes.size());
    for (auto node : nodes) {
        DEBUG_INFO("  Node: %u%s", node, (node == rootId) ? " (ROOT)" : "");
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

        setLedColor(255, 0, 255);  // Magenta flash
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

void statusPrint(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        DEBUG_INFO("\n--- Status Report ---");
        DEBUG_INFO("Device ID: %u", deviceId);
        DEBUG_INFO("Firmware MD5: %s", fw_md5.c_str());
        DEBUG_INFO("Root ID: %u", rootId);
        DEBUG_INFO("Registered: %s", registeredWithRoot ? "Yes" : "No");
        DEBUG_INFO("Free Heap: %d bytes", ESP.getFreeHeap());
        DEBUG_INFO("Uptime: %lu seconds", millis() / 1000);
        DEBUG_INFO("WiFi RSSI: %d dBm", WiFi.RSSI());
        DEBUG_INFO("Dropped messages: %u", stats.meshDropped);

        // Print connections stats
        printConnectionsStats();

        printNodes();
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

void handleButtonsTask(void* pvParameters) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));

        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        unsigned long currentMillis = millis();

        for (int i = 0; i < NLIGHTS; i++) {
            int currentState = digitalRead(buttonPins[i]);

            if (currentMillis - lastTimeClick[i] < BUTTON_DEBOUNCE_TIME) {
                continue;
            }

            if (currentState == HIGH && lastButtonState[i] == LOW) {
                lastTimeClick[i] = currentMillis;
                clicks++;

                char button = 'a' + i;
                DEBUG_VERBOSE("BUTTON: Button %d pressed ('%c')", i, button);

                if (mesh.getNodeList().empty()) {
                    DEBUG_ERROR("BUTTON: No mesh connection");
                    setLedColor(255, 0, 0);
                    vTaskDelay(100 / portTICK_PERIOD_MS);
                    continue;
                }

                // Get targets from configuration
                auto targets = getTargetsForButton(button);

                if (targets.empty()) {
                    DEBUG_VERBOSE(
                        "BUTTON: No targets configured for button '%c'",
                        button);
                    continue;
                }
                DEBUG_INFO("BUTTON: Sending to %d targets", targets.size());

                for (const auto& target : targets) {
                    uint32_t targetId = target.first.toInt();
                    String command = target.second;

                    DEBUG_VERBOSE("BUTTON: -> Node %u: %s", targetId,
                                  command.c_str());

                    safePush(meshPriorityQueue,
                             std::make_pair(targetId, command),
                             meshPriorityQueueMutex, stats.meshDropped,
                             "MESH-PRIORITY");

                    safePush(meshMessageQueue, std::make_pair(rootId, command),
                             meshMessageQueueMutex, stats.meshDropped,
                             "MESH-MSG");
                }

                // Flash LED to confirm
                setLedColor(0, 255, 255);
                vTaskDelay(50 / portTICK_PERIOD_MS);
            }

            lastButtonState[i] = currentState;
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
            if (rootId == 0) {
                DEBUG_VERBOSE("MESH: Root ID unknown, cannot register");
                continue;
            }
            DEBUG_INFO("MESH: Attempting registration with root...");
            safePush(meshMessageQueue, std::make_pair(rootId, String("S")),
                     meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
            DEBUG_VERBOSE("MESH: Sent registration 'S' to root %u", rootId);
        }
    }
}

void onNewConnection(uint32_t nodeId) {
    DEBUG_INFO("MESH: New connection from node %u", nodeId);
    if (rootId == 0) {
        DEBUG_ERROR("MESH: Root ID unknown, cannot register");
        return;
    }
    safePush(meshMessageQueue, std::make_pair(rootId, String("S")),
             meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
    DEBUG_VERBOSE("MESH: Sent registration 'S' to root %u", rootId);
    printNodes();
}

void onDroppedConnection(uint32_t nodeId) {
    DEBUG_ERROR("MESH: Dropped connection to node %u", nodeId);
    DEBUG_ERROR("MESH: Lost connection to root, resetting");
    registeredWithRoot = false;
    disconnects++;
}

void meshCallbackTask(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        std::pair<uint32_t, String> message;
        if (!safePop(meshCallbackQueue, message, meshCallbackQueueMutex)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // No artificial delay for processing
        uint32_t from = message.first;
        String msg = message.second;

        if (msg.startsWith("{")) {
            DEBUG_INFO("MESH: Received connections configuration from %u",
                       from);
            receiveConnections(msg);
            continue;
        }

        if (msg == "U") {
            DEBUG_INFO("MESH: Firmware update command received");
            otaTimerStarted = true;
            setLedColor(0, 0, 255);
            continue;
        }

        if (msg == "Q") {
            DEBUG_VERBOSE("MESH: Registration query received from root");
            rootId = from;
            safePush(meshMessageQueue, std::make_pair(rootId, String("S")),
                     meshMessageQueueMutex, stats.meshDropped, "MESH-MSG");
            DEBUG_VERBOSE("MESH: Sent registration 'S' to root %u", rootId);
            continue;
        }

        if (msg == "A") {
            DEBUG_INFO("MESH: Registration accepted by root");
            registeredWithRoot = true;
            continue;
        }

        DEBUG_ERROR("MESH: Unknown message from %u: %s", from, msg.c_str());
    }
}

void sendMeshMessages(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Check priority queue FIRST for button presses (no delay)
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
    vTaskDelay(pdMS_TO_TICKS(1000));
    fw_md5 = ESP.getSketchMD5();
    DEBUG_INFO("\n\n========================================");
    DEBUG_INFO("ESP32-C3 Mesh Switch Node Starting...");
    DEBUG_INFO("Chip Model: %s", ESP.getChipModel());
    DEBUG_INFO("Sketch MD5: %s", fw_md5.c_str());
    DEBUG_INFO("Chip Revision: %d", ESP.getChipRevision());
    DEBUG_INFO("CPU Frequency: %d MHz", ESP.getCpuFreqMHz());
    DEBUG_INFO("Free Heap: %d bytes", ESP.getFreeHeap());
    DEBUG_INFO("Flash Size: %d bytes", ESP.getFlashChipSize());
    DEBUG_INFO("Time To Reset: %d", RESET_TIMEOUT - (millis() - resetTimer));
    DEBUG_INFO("========================================\n");

    // CRITICAL: Create mutexes BEFORE meshInit()
    DEBUG_INFO("Creating mutexes...");
    meshCallbackQueueMutex = xSemaphoreCreateMutex();
    meshMessageQueueMutex = xSemaphoreCreateMutex();
    meshPriorityQueueMutex = xSemaphoreCreateMutex();
    myConnectionsMutex = xSemaphoreCreateMutex();

    if (!meshCallbackQueueMutex || !meshMessageQueueMutex ||
        !meshPriorityQueueMutex || !myConnectionsMutex) {
        DEBUG_ERROR("FATAL: Failed to create mutexes!");
        ESP.restart();
    }
    DEBUG_INFO("All mutexes created successfully");

    loadConnectionsOnBoot();

    // Initialize NeoPixel
    pixels.begin();
    pixels.setBrightness(5);
    setLedColor(255, 0, 0);  // Red on startup

    // Initialize mesh
    meshInit();

    // Setup button pins
    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(buttonPins[i], INPUT_PULLDOWN);
    }

    // Start tasks with optimized priorities
    DEBUG_INFO("Creating tasks...");
    xTaskCreatePinnedToCore(handleButtonsTask, "ButtonTask", 4096, NULL, 2,
                            NULL, 0);
    xTaskCreatePinnedToCore(updateLedStatus, "LedTask", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(statusPrint, "StatusPrintTask", 4096, NULL, 1, NULL,
                            0);
    xTaskCreatePinnedToCore(resetTask, "ResetTask", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(sendStatusReport, "SendStatusReportTask", 4096,
                            NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(registerTask, "RegisterTask", 4096, NULL, 1, NULL,
                            0);
    xTaskCreatePinnedToCore(meshCallbackTask, "MeshCallbackTask", 8192, NULL, 4,
                            NULL, 0);
    xTaskCreatePinnedToCore(sendMeshMessages, "SendMeshMessages", 8192, NULL, 3,
                            NULL, 0);  // Higher priority

    DEBUG_INFO("SWITCH: Setup complete, waiting for mesh connections...");
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