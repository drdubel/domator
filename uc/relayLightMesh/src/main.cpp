#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
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
#define RESET_TIMEOUT 60000

// Queue size limits
#define MAX_QUEUE_SIZE 40
#define CRITICAL_HEAP_THRESHOLD 25000
#define LOW_HEAP_THRESHOLD 50000

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
std::queue<std::pair<uint32_t, String>>
    meshPriorityQueue;  // Fast path for relay commands

// Mutexes for thread safety
SemaphoreHandle_t meshCallbackQueueMutex = NULL;
SemaphoreHandle_t meshMessageQueueMutex = NULL;
SemaphoreHandle_t meshPriorityQueueMutex = NULL;
SemaphoreHandle_t lightsArrayMutex = NULL;  // Protect lights[] array

// Statistics
struct Statistics {
    uint32_t meshDropped = 0;
    uint32_t lowHeapEvents = 0;
    uint32_t criticalHeapEvents = 0;
} stats;

bool registeredWithRoot = false;
uint32_t resetTimer = 0;
uint32_t otaTimer = 0;
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

void otaTask(void* pv) {
    otaInProgress = true;
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

void restartMesh() {
    DEBUG_ERROR("MESH: Restarting ESP32 due to mesh timeout");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP.restart();
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
        if ((millis() - resetTimer) > RESET_TIMEOUT) restartMesh();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void buttonPressTask(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (pressed) {
            for (int i = 0; i < NLIGHTS; i++) {
                if (pressed & (1 << i)) {
                    DEBUG_VERBOSE("RELAY: Button for light %d pressed", i);
                    String msg = String(char('a' + i)) +
                                 String(char(buttonState[i] + '0'));
                    safePush(meshMessageQueue, std::make_pair(rootId, msg),
                             meshMessageQueueMutex, stats.meshDropped,
                             "MESH-MSG");
                    pressed &= ~(1 << i);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
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
        otaTimer = millis();
        otaInProgress = true;
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

    if (!meshCallbackQueueMutex || !meshMessageQueueMutex ||
        !meshPriorityQueueMutex || !lightsArrayMutex) {
        DEBUG_ERROR("FATAL: Failed to create mutexes!");
        ESP.restart();
    }
    DEBUG_INFO("All mutexes created successfully");

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
    if (otaInProgress && !otaTaskStarted && millis() - otaTimer > 3000) {
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