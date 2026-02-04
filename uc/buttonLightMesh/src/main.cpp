#include <Adafruit_NeoPixel.h>
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
#define ESPNOW_CHANNEL 1

// Queue size limits
#define MAX_QUEUE_SIZE 30
#define CRITICAL_HEAP_THRESHOLD 20000
#define LOW_HEAP_THRESHOLD 40000

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
    uint32_t networkId;  // Network identifier derived from MESH_PASSWORD hash
    uint8_t msgType;     // 'Q'=query, 'S'=switch registration, 'A'=ack,
                         // 'U'=update, 'D'=data, 'C'=command
    char data[196];      // Reduced by 4 bytes for networkId
} espnow_message_t;

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

uint8_t rootMac[6] = {0};  // Root node MAC address
uint8_t espnowChannel = 1;
uint32_t deviceId = 0;
uint32_t networkId = 0;  // Network identifier derived from MESH_PASSWORD hash
uint32_t disconnects = 0;
uint32_t clicks = 0;
String fw_md5;

// Queues
std::queue<espnow_message_t> espnowCallbackQueue;
std::queue<std::pair<String, bool>> espnowMessageQueue;  // message, isPriority

// Mutexes
SemaphoreHandle_t espnowCallbackQueueMutex = NULL;
SemaphoreHandle_t espnowMessageQueueMutex = NULL;

// Statistics
struct Statistics {
    uint32_t espnowDropped = 0;
    uint32_t lowHeapEvents = 0;
    uint32_t criticalHeapEvents = 0;
    uint32_t espnowSendFailed = 0;
    uint32_t espnowSendSuccess = 0;
} stats;

const int buttonPins[NLIGHTS] = {A0, A1, A3, A4, A5, 6, 7};
uint32_t lastTimeClick[NLIGHTS] = {0};
int lastButtonState[NLIGHTS] = {HIGH};

// State variables
bool registeredWithRoot = false;
bool hasRootMac = false;
uint32_t resetTimer = 0;
uint32_t otaTimer = 0;
bool otaTimerStarted = false;
volatile bool otaInProgress = false;
unsigned long lastRootComm = 0;

// Forward declarations
void performFirmwareUpdate();
void setLedColor(uint8_t r, uint8_t g, uint8_t b);
void onESPNowDataSent(const uint8_t* mac_addr, esp_now_send_status_t status);
void onESPNowDataRecv(const uint8_t* mac_addr, const uint8_t* data, int len);

// Compute network ID from MESH_PASSWORD to isolate different mesh networks
uint32_t computeNetworkId(const char* password) {
    unsigned char hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // SHA-256 (not SHA-224)
    mbedtls_sha256_update(&ctx, (const unsigned char*)password, strlen(password));
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    
    // Use first 4 bytes of hash as network ID
    return (hash[0] << 24) | (hash[1] << 16) | (hash[2] << 8) | hash[3];
}

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

        if (hasRootMac && registeredWithRoot) {
            setLedColor(0, 255, 0);  // Green - fully connected
        } else if (hasRootMac) {
            setLedColor(255, 255,
                        0);  // Yellow - has root MAC, waiting for registration
        } else {
            setLedColor(255, 0, 0);  // Red - not connected
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void otaTask(void* pv) {
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

bool sendESPNowMessage(const String& message, bool priority = false,
                       const uint8_t* destMac = nullptr) {
    if (!destMac && !hasRootMac) {
        DEBUG_ERROR("Cannot send: No root MAC address");
        return false;
    }

    destMac = destMac ? destMac : rootMac;

    // Add root as peer if not already added
    if (!esp_now_is_peer_exist(destMac)) {
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, destMac, 6);
        peerInfo.channel = espnowChannel;  // Use the channel we're on!
        peerInfo.encrypt = false;

        esp_err_t result = esp_now_add_peer(&peerInfo);
        if (result != ESP_OK) {
            DEBUG_ERROR("Failed to add root as peer: %d", result);
            return false;
        }
        DEBUG_INFO("Added root as ESP-NOW peer on channel %d", espnowChannel);
    }

    espnow_message_t msg;
    msg.nodeId = deviceId;
    msg.networkId = networkId;  // Include network ID for isolation

    // Determine message type
    if (message == "S") {
        msg.msgType = 'S';
    } else if (message.startsWith("{")) {
        msg.msgType = 'D';
    } else {
        msg.msgType = 'C';
    }

    strncpy(msg.data, message.c_str(), sizeof(msg.data) - 1);
    msg.data[sizeof(msg.data) - 1] = '\0';

    esp_err_t result =
        esp_now_send(destMac, (uint8_t*)&msg, sizeof(espnow_message_t));

    if (result == ESP_OK) {
        DEBUG_VERBOSE("ESP-NOW TX: type=%c data=%s", msg.msgType, msg.data);
        return true;
    } else {
        DEBUG_ERROR("ESP-NOW TX failed: %d", result);
        stats.espnowSendFailed++;
        return false;
    }
}

// ESP32-C3 compatible callback signature
void onESPNowDataRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
    if (len != sizeof(espnow_message_t)) {
        DEBUG_ERROR("ESP-NOW: Invalid message size: %d", len);
        return;
    }

    espnow_message_t msg;
    memcpy(&msg, data, sizeof(espnow_message_t));

    // Validate network ID to prevent cross-network interference
    if (msg.networkId != networkId) {
        DEBUG_VERBOSE("ESP-NOW: Rejected msg from different network (ID: 0x%08X, expected: 0x%08X)",
                      msg.networkId, networkId);
        return;
    }

    // Learn root MAC on discovery 'Q'; finalize on ACK 'A'
    if (!hasRootMac) {
        String dataStr = String(msg.data);
        if (msg.msgType == 'Q' || dataStr == "Q") {
            DEBUG_INFO("Attempting registration with root...");
            String regMessage = String(MESH_PASSWORD) + ":S";
            uint8_t tempRootMac[6] = {0};
            memcpy(tempRootMac, mac_addr, 6);
            sendESPNowMessage(regMessage, false, tempRootMac);
        } else if (msg.msgType == 'A' || dataStr == "A") {
            memcpy(rootMac, mac_addr, 6);
            hasRootMac = true;
            registeredWithRoot = true;
            DEBUG_INFO("ESP-NOW: Registered with root MAC");
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

    // Compute network ID from MESH_PASSWORD for network isolation
    networkId = computeNetworkId(MESH_PASSWORD);
    DEBUG_INFO("Network ID: 0x%08X (derived from MESH_PASSWORD)", networkId);

    // Set WiFi mode but don't connect
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Scan to find what channel the WiFi network is on
    DEBUG_INFO("Scanning for WiFi network '%s'...", WIFI_SSID);
    int n = WiFi.scanNetworks();
    uint8_t channel = 1;  // Default
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

    // SAVE THE CHANNEL GLOBALLY
    espnowChannel = channel;

    // Get device MAC and generate ID
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    deviceId = (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5];

    DEBUG_INFO("Device MAC: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
               mac[2], mac[3], mac[4], mac[5]);
    DEBUG_INFO("Device ID: %u", deviceId);

    // Set channel to match WiFi network
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    DEBUG_INFO("Set ESP-NOW to channel %d", channel);

    // Initialize ESP-NOW
    if (esp_now_init() != ESP_OK) {
        DEBUG_ERROR("ESP-NOW init failed");
        return;
    }

    DEBUG_INFO(
        "ESP-NOW initialized - waiting for root discovery (encryption "
        "disabled)");

    // Register callbacks
    esp_now_register_send_cb(onESPNowDataSent);
    esp_now_register_recv_cb(onESPNowDataRecv);
}

void sendStatusReport(void* pvParameters) {
    while (true) {
        if (otaInProgress || !hasRootMac || !registeredWithRoot) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        DEBUG_VERBOSE("Sending status report to root");
        JsonDocument doc;
        doc["rssi"] = WiFi.RSSI();
        doc["uptime"] = millis() / 1000;
        doc["clicks"] = clicks;
        doc["disconnects"] = disconnects;
        doc["parentId"] = 0;  // Always report to root in star topology
        doc["deviceId"] = deviceId;
        doc["freeHeap"] = ESP.getFreeHeap();
        doc["type"] = "switch";
        doc["firmware"] = fw_md5;
        doc["lowHeap"] = stats.lowHeapEvents;

        String msg;
        serializeJson(doc, msg);

        safePush(espnowMessageQueue, std::make_pair(msg, false),
                 espnowMessageQueueMutex, stats.espnowDropped, "ESPNOW-MSG");

        vTaskDelay(pdMS_TO_TICKS(STATUS_REPORT_INTERVAL));
    }
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
        DEBUG_INFO("Has Root MAC: %s", hasRootMac ? "Yes" : "No");
        DEBUG_INFO("Registered: %s", registeredWithRoot ? "Yes" : "No");
        DEBUG_INFO("Free Heap: %d bytes", ESP.getFreeHeap());
        DEBUG_INFO("Uptime: %lu seconds", millis() / 1000);
        DEBUG_INFO("Dropped messages: %u", stats.espnowDropped);
        DEBUG_INFO("Send success: %u, failed: %u", stats.espnowSendSuccess,
                   stats.espnowSendFailed);
        DEBUG_INFO("Last root comm: %lu ms ago", millis() - lastRootComm);
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

        // Reset if no communication with root for too long
        if (hasRootMac && registeredWithRoot) {
            if (millis() - lastRootComm > RESET_TIMEOUT) {
                DEBUG_ERROR("No communication with root for %d ms, restarting",
                            RESET_TIMEOUT);
                ESP.restart();
            }
        }

        if (!otaTimerStarted) {
            otaTimer = millis();
        } else if ((millis() - otaTimer) > OTA_START_DELAY) {
            otaInProgress = true;
        }

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

                if (!hasRootMac) {
                    DEBUG_ERROR("BUTTON: No root connection");
                    setLedColor(255, 0, 0);
                    vTaskDelay(100 / portTICK_PERIOD_MS);
                    continue;
                }

                // Send button press to root immediately
                String buttonMsg = String(button);
                safePush(espnowMessageQueue, std::make_pair(buttonMsg, true),
                         espnowMessageQueueMutex, stats.espnowDropped,
                         "ESPNOW-PRIORITY");

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

        if (!registeredWithRoot && hasRootMac) {
            DEBUG_INFO("Attempting registration with root...");
            String regMessage = String(MESH_PASSWORD) + ":S";
            sendESPNowMessage(regMessage, false);
            DEBUG_VERBOSE("Sent registration request with password to root");
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
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        String msg = String(message.data);

        if (msg == "U") {
            DEBUG_INFO("Firmware update command received");
            otaTimerStarted = true;
            setLedColor(0, 0, 255);
            continue;
        }

        if (msg == "Q") {
            DEBUG_VERBOSE(
                "Registration query received from root (registered=%d)",
                registeredWithRoot);
            // Always respond to re-authenticate after root restart
            String regMessage = String(MESH_PASSWORD) + ":S";
            sendESPNowMessage(regMessage, false);
            DEBUG_VERBOSE("Sent registration request to root");
            continue;
        }

        if (msg == "A") {
            DEBUG_INFO("Registration accepted by root");
            registeredWithRoot = true;
            lastRootComm = millis();
            continue;
        }

        DEBUG_ERROR("Unknown message: %s", msg.c_str());
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

        // Minimal delay for priority messages (button presses)
        vTaskDelay(pdMS_TO_TICKS(priority ? 2 : 5));
    }
}

void setup() {
    Serial.begin(115200);
    vTaskDelay(pdMS_TO_TICKS(1000));

    fw_md5 = ESP.getSketchMD5();
    DEBUG_INFO("\n\n========================================");
    DEBUG_INFO("ESP32-C3 ESP-NOW Switch Node Starting...");
    DEBUG_INFO("Chip Model: %s", ESP.getChipModel());
    DEBUG_INFO("Sketch MD5: %s", fw_md5.c_str());
    DEBUG_INFO("Chip Revision: %d", ESP.getChipRevision());
    DEBUG_INFO("CPU Frequency: %d MHz", ESP.getCpuFreqMHz());
    DEBUG_INFO("Free Heap: %d bytes", ESP.getFreeHeap());
    DEBUG_INFO("Flash Size: %d bytes", ESP.getFlashChipSize());
    DEBUG_INFO("========================================\n");

    // Create mutexes
    DEBUG_INFO("Creating mutexes...");
    espnowCallbackQueueMutex = xSemaphoreCreateMutex();
    espnowMessageQueueMutex = xSemaphoreCreateMutex();

    if (!espnowCallbackQueueMutex || !espnowMessageQueueMutex) {
        DEBUG_ERROR("FATAL: Failed to create mutexes!");
        ESP.restart();
    }
    DEBUG_INFO("All mutexes created successfully");

    // Initialize NeoPixel
    pixels.begin();
    pixels.setBrightness(5);
    setLedColor(255, 0, 0);  // Red on startup

    // Initialize ESP-NOW
    espnowInit();

    // Setup button pins
    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(buttonPins[i], INPUT_PULLDOWN);
    }

    // Start tasks
    DEBUG_INFO("Creating tasks...");
    xTaskCreatePinnedToCore(handleButtonsTask, "ButtonTask", 4096, NULL, 3,
                            NULL, 0);
    xTaskCreatePinnedToCore(updateLedStatus, "LedTask", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(statusPrint, "StatusPrintTask", 4096, NULL, 1, NULL,
                            0);
    xTaskCreatePinnedToCore(resetTask, "ResetTask", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(sendStatusReport, "SendStatusReportTask", 4096,
                            NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(registerTask, "RegisterTask", 4096, NULL, 1, NULL,
                            0);
    xTaskCreatePinnedToCore(espnowCallbackTask, "ESPNowCallbackTask", 8192,
                            NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(sendESPNowMessages, "SendESPNowMessages", 8192,
                            NULL, 4, NULL, 0);

    DEBUG_INFO("SWITCH: Setup complete, waiting for ESP-NOW connections...");
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