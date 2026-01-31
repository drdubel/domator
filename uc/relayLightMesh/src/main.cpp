#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <credentials.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>

#include <map>
#include <queue>
#include <vector>

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
#define OTA_START_DELAY 5000

// Queue size limits
#define MAX_QUEUE_SIZE 40
#define CRITICAL_HEAP_THRESHOLD 25000
#define LOW_HEAP_THRESHOLD 50000

// Debug levels
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

// ESP-NOW message structure
typedef struct __attribute__((packed)) {
    uint32_t nodeId;
    uint8_t msgType;  // 'Q'=query, 'R'=relay registration, 'A'=ack, 'U'=update,
                      // 'D'=data, 'C'=command
    char data[200];
} espnow_message_t;

uint8_t rootMac[6] = {0};   // Root node MAC address
uint8_t espnowChannel = 1;  // Current ESP-NOW channel
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
std::queue<espnow_message_t> espnowCallbackQueue;
std::queue<std::pair<String, bool>> espnowMessageQueue;  // message, isPriority

// Mutexes
SemaphoreHandle_t espnowCallbackQueueMutex = NULL;
SemaphoreHandle_t espnowMessageQueueMutex = NULL;
SemaphoreHandle_t lightsArrayMutex = NULL;

// Statistics
struct Statistics {
    uint32_t espnowDropped = 0;
    uint32_t lowHeapEvents = 0;
    uint32_t criticalHeapEvents = 0;
    uint32_t espnowSendFailed = 0;
    uint32_t espnowSendSuccess = 0;
} stats;

// State variables
bool registeredWithRoot = false;
bool hasRootMac = false;
uint32_t otaTimer = 0;
volatile bool otaInProgress = false;
unsigned long lastRootComm = 0;

// Forward declarations
void performFirmwareUpdate();
void onESPNowDataSent(const uint8_t* mac_addr, esp_now_send_status_t status);
void onESPNowDataRecv(const uint8_t* mac_addr, const uint8_t* data, int len);

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
        DEBUG_ERROR("CRITICAL: Low heap %u bytes! Clearing queues...",
                    freeHeap);
        if (xSemaphoreTake(espnowMessageQueueMutex, pdMS_TO_TICKS(100)) ==
            pdTRUE) {
            while (!espnowMessageQueue.empty()) espnowMessageQueue.pop();
            xSemaphoreGive(espnowMessageQueueMutex);
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
    DEBUG_INFO("[OTA] Stopping ESP-NOW...");
    esp_now_deinit();
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

void onESPNowDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        stats.espnowSendSuccess++;
        lastRootComm = millis();
    } else {
        stats.espnowSendFailed++;
        DEBUG_VERBOSE("ESP-NOW send failed to root");
    }
}

void onESPNowDataRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
    if (len != sizeof(espnow_message_t)) {
        DEBUG_ERROR("ESP-NOW: Invalid message size: %d", len);
        return;
    }

    espnow_message_t msg;
    memcpy(&msg, data, sizeof(espnow_message_t));

    // Learn root MAC on discovery 'Q'; finalize on ACK 'A'
    if (!hasRootMac) {
        String dataStr = String(msg.data);
        if (msg.msgType == 'Q' || dataStr == "Q") {
            memcpy(rootMac, mac_addr, 6);
            hasRootMac = true;
            DEBUG_VERBOSE("ESP-NOW: Discovered root MAC");
        } else if (msg.msgType == 'A' || dataStr == "A") {
            memcpy(rootMac, mac_addr, 6);
            hasRootMac = true;
            registeredWithRoot = true;
            DEBUG_INFO(
                "ESP-NOW: Registered with root MAC: "
                "%02X:%02X:%02X:%02X:%02X:%02X",
                rootMac[0], rootMac[1], rootMac[2], rootMac[3], rootMac[4],
                rootMac[5]);
        }
    }

    lastRootComm = millis();

    DEBUG_VERBOSE("ESP-NOW RX: [%u] type=%c data=%s", msg.nodeId, msg.msgType,
                  msg.data);

    if (!checkHeapHealth()) {
        stats.espnowDropped++;
        return;
    }

    safePush(espnowCallbackQueue, msg, espnowCallbackQueueMutex,
             stats.espnowDropped, "ESPNOW-CB");
}

void espnowInit() {
    DEBUG_INFO("Initializing ESP-NOW...");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Scan for WiFi network to find channel
    DEBUG_INFO("Scanning for WiFi network '%s'...", WIFI_SSID);
    int n = WiFi.scanNetworks();
    uint8_t channel = 1;
    bool found = false;

    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == String(WIFI_SSID)) {
            channel = WiFi.channel(i);
            found = true;
            DEBUG_INFO("Found network '%s' on channel %d", WIFI_SSID, channel);
            break;
        }
    }
    WiFi.scanDelete();

    if (!found) {
        DEBUG_ERROR("WiFi network '%s' not found! Using default channel %d",
                    WIFI_SSID, channel);
    }

    espnowChannel = channel;

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    deviceId = (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5];

    DEBUG_INFO("Device MAC: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
               mac[2], mac[3], mac[4], mac[5]);
    DEBUG_INFO("Device ID: %u", deviceId);

    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    DEBUG_INFO("ESP-NOW using channel %d", channel);

    if (esp_now_init() != ESP_OK) {
        DEBUG_ERROR("ESP-NOW init failed");
        return;
    }

    // Derive PMK from password using SHA-256
    uint8_t hash[32];
    uint8_t pmk[16];

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // 0 = SHA-256 (not SHA-224)
    mbedtls_sha256_update(&ctx, (const unsigned char*)MESH_PASSWORD,
                          strlen(MESH_PASSWORD));
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    // Use first 16 bytes as PMK
    memcpy(pmk, hash, 16);

    esp_now_set_pmk(pmk);
    DEBUG_INFO("ESP-NOW encryption enabled (SHA-256 derived key)");

    DEBUG_INFO("ESP-NOW initialized successfully");

    esp_now_register_send_cb(onESPNowDataSent);
    esp_now_register_recv_cb(onESPNowDataRecv);
}

bool sendESPNowMessage(const String& message, bool priority = false) {
    if (!hasRootMac) {
        DEBUG_ERROR("Cannot send: No root MAC address");
        return false;
    }

    if (!esp_now_is_peer_exist(rootMac)) {
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, rootMac, 6);
        peerInfo.channel = espnowChannel;
        peerInfo.encrypt = true;

        // Derive LMK from password using SHA-256
        uint8_t hash[32];
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, (const unsigned char*)MESH_PASSWORD,
                              strlen(MESH_PASSWORD));
        mbedtls_sha256_finish(&ctx, hash);
        mbedtls_sha256_free(&ctx);
        memcpy(peerInfo.lmk, hash, 16);

        esp_err_t result = esp_now_add_peer(&peerInfo);
        if (result != ESP_OK) {
            DEBUG_ERROR("Failed to add root peer: %d", result);
            return false;
        }
        DEBUG_INFO("Added root as ESP-NOW peer on channel %d", espnowChannel);
    }

    espnow_message_t msg;
    msg.nodeId = deviceId;

    if (message == "R") {
        msg.msgType = 'R';
    } else if (message.startsWith("{")) {
        msg.msgType = 'D';
    } else if (message.length() == 2 && message[0] >= 'A' &&
               message[0] <= 'H') {
        msg.msgType = 'C';
    } else {
        msg.msgType = 'D';
    }

    strncpy(msg.data, message.c_str(), sizeof(msg.data) - 1);
    msg.data[sizeof(msg.data) - 1] = '\0';

    esp_err_t result =
        esp_now_send(rootMac, (uint8_t*)&msg, sizeof(espnow_message_t));

    if (result == ESP_OK) {
        DEBUG_VERBOSE("ESP-NOW TX: type=%c data=%s%s", msg.msgType, msg.data,
                      priority ? " [PRI]" : "");
        return true;
    } else {
        DEBUG_ERROR("ESP-NOW TX failed: %d", result);
        stats.espnowSendFailed++;
        return false;
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

            safePush(espnowMessageQueue, std::make_pair(String(message), true),
                     espnowMessageQueueMutex, stats.espnowDropped,
                     "ESPNOW-PRI");
        }
        xSemaphoreGive(lightsArrayMutex);
    }
}

void sendStatusReport(void* pvParameters) {
    while (true) {
        if (otaInProgress || !hasRootMac || !registeredWithRoot) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        JsonDocument doc;
        doc["rssi"] = 0;
        doc["uptime"] = millis() / 1000;
        doc["clicks"] = clicks;
        doc["disconnects"] = disconnects;
        doc["parentId"] = 0;
        doc["deviceId"] = deviceId;
        doc["freeHeap"] = ESP.getFreeHeap();
        doc["type"] = "relay";
        doc["firmware"] = fw_md5;
        doc["lowHeap"] = stats.lowHeapEvents;

        String msg;
        serializeJson(doc, msg);

        safePush(espnowMessageQueue, std::make_pair(msg, false),
                 espnowMessageQueueMutex, stats.espnowDropped, "ESPNOW-MSG");

        vTaskDelay(pdMS_TO_TICKS(STATUS_REPORT_INTERVAL));
    }
}

void statusPrintTask(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        DEBUG_INFO("\n--- Relay Status ---");
        DEBUG_INFO("Device ID: %u", deviceId);
        DEBUG_INFO("Has Root: %s", hasRootMac ? "Yes" : "No");
        DEBUG_INFO("Registered: %s", registeredWithRoot ? "Yes" : "No");
        DEBUG_INFO("Free Heap: %d bytes", ESP.getFreeHeap());
        DEBUG_INFO("Uptime: %lu s", millis() / 1000);
        DEBUG_INFO("-------------------\n");

        vTaskDelay(pdMS_TO_TICKS(STATUS_PRINT_INTERVAL));
    }
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

            if (xSemaphoreTake(lightsArrayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                lights[i] = buttonState[i] ? HIGH : LOW;
                clicks++;

                char response[3];
                response[0] = 'a' + i;
                response[1] = lights[i] ? '1' : '0';
                response[2] = '\0';

                xSemaphoreGive(lightsArrayMutex);

                safePush(
                    espnowMessageQueue, std::make_pair(String(response), true),
                    espnowMessageQueueMutex, stats.espnowDropped, "ESPNOW-PRI");

                DEBUG_INFO("Light %c set to %s by button", button,
                           lights[i] ? "ON" : "OFF");
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

        if (!registeredWithRoot && hasRootMac) {
            DEBUG_INFO("Attempting registration with root...");
            digitalWrite(23, LOW);
            String regMessage = String(MESH_PASSWORD) + ":R";
            sendESPNowMessage(regMessage, false);
        } else if (registeredWithRoot) {
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

void processMeshMessage(const espnow_message_t& message) {
    String msg = String(message.data);

    if (msg == "S") {
        DEBUG_INFO("Root requesting state sync");
        syncLightStates();
        return;
    }

    if (msg == "Q") {
        DEBUG_VERBOSE("Registration query received from root (registered=%d)",
                      registeredWithRoot);
        // Always respond to re-authenticate after root restart
        String regMessage = String(MESH_PASSWORD) + ":R";
        sendESPNowMessage(regMessage, false);
        DEBUG_VERBOSE("Sent registration request to root");
        return;
    }

    if (msg == "U") {
        DEBUG_INFO("Firmware update command received");
        otaInProgress = true;
        return;
    }

    // Handle light control with state (e.g., "a0", "b1")
    if (msg.length() == 2 && msg[0] >= 'a' && msg[0] < 'a' + NLIGHTS) {
        int lightIndex = msg[0] - 'a';
        int newState = msg[1] - '0';

        if (newState != 0 && newState != 1) {
            return;
        }

        if (xSemaphoreTake(lightsArrayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            lights[lightIndex] = newState;
            digitalWrite(relays[lightIndex], newState ? HIGH : LOW);
            clicks++;

            char response[3];
            response[0] = 'A' + lightIndex;
            response[1] = newState ? '1' : '0';
            response[2] = '\0';

            xSemaphoreGive(lightsArrayMutex);

            safePush(espnowMessageQueue, std::make_pair(String(response), true),
                     espnowMessageQueueMutex, stats.espnowDropped,
                     "ESPNOW-PRI");
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

            char response[3];
            response[0] = 'A' + lightIndex;
            response[1] = lights[lightIndex] ? '1' : '0';
            response[2] = '\0';

            xSemaphoreGive(lightsArrayMutex);

            safePush(espnowMessageQueue, std::make_pair(String(response), true),
                     espnowMessageQueueMutex, stats.espnowDropped,
                     "ESPNOW-PRI");
        }
        return;
    }

    if (msg == "A") {
        DEBUG_INFO("Registration accepted by root");
        registeredWithRoot = true;
        lastRootComm = millis();
        return;
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
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        processMeshMessage(message);
    }
}

void sendESPNowMessages(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        std::pair<String, bool> message;
        if (!safePop(espnowMessageQueue, message, espnowMessageQueueMutex)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        String msg = message.first;
        bool priority = message.second;

        sendESPNowMessage(msg, priority);

        vTaskDelay(pdMS_TO_TICKS(priority ? 2 : 5));
    }
}

void setup() {
    Serial.begin(115200);
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    fw_md5 = ESP.getSketchMD5();
    DEBUG_INFO("\n========================================");
    DEBUG_INFO("ESP32 ESP-NOW Relay Node Starting...");
    DEBUG_INFO("Sketch MD5: %s", fw_md5.c_str());
    DEBUG_INFO("Free Heap: %d bytes", ESP.getFreeHeap());
    DEBUG_INFO("========================================\n");

    DEBUG_INFO("Creating mutexes...");
    espnowCallbackQueueMutex = xSemaphoreCreateMutex();
    espnowMessageQueueMutex = xSemaphoreCreateMutex();
    lightsArrayMutex = xSemaphoreCreateMutex();

    if (!espnowCallbackQueueMutex || !espnowMessageQueueMutex ||
        !lightsArrayMutex) {
        DEBUG_ERROR("FATAL: Failed to create mutexes!");
        ESP.restart();
    }

    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(relays[i], OUTPUT);
        digitalWrite(relays[i], LOW);
    }

    pinMode(23, OUTPUT);
    digitalWrite(23, LOW);

    espnowInit();

    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(buttons[i], INPUT_PULLDOWN);
        attachInterruptArg(digitalPinToInterrupt(buttons[i]), buttonISR,
                           (void*)i, CHANGE);
    }

    DEBUG_INFO("Creating tasks...");
    xTaskCreatePinnedToCore(statusPrintTask, "Status", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(sendStatusReport, "StatusRpt", 8192, NULL, 1, NULL,
                            1);
    xTaskCreatePinnedToCore(registerTask, "Register", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(buttonPressTask, "Button", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(espnowCallbackTask, "ESPNowCB", 8192, NULL, 4, NULL,
                            0);
    xTaskCreatePinnedToCore(sendESPNowMessages, "ESPNowTX", 8192, NULL, 4, NULL,
                            0);

    DEBUG_INFO("Setup complete");
}

void loop() {
    static bool otaTaskStarted = false;

    if (otaInProgress && !otaTaskStarted) {
        otaTaskStarted = true;
        DEBUG_INFO("[OTA] Starting OTA task...");
        xTaskCreatePinnedToCore(otaTask, "OTA", 8192, NULL, 5, NULL, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
}