#include <Adafruit_NeoPixel.h>
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

// Function declarations
void receivedCallback(const uint32_t& from, const String& msg);
void meshInit();
void performFirmwareUpdate();
void setLedColor(uint8_t r, uint8_t g, uint8_t b);
void updateLedStatus();
void printNodes();

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

painlessMesh mesh;

uint32_t rootId = 0;
uint32_t deviceId = 0;
uint32_t disconnects = 0;
uint32_t clicks = 0;

String fw_md5;  // MD5 of the firmware as flashed

const int buttonPins[NLIGHTS] = {A0, A1, A3, A4, A5, 6, 7};
volatile uint32_t lastPress[NLIGHTS] = {0};
volatile uint8_t pressed = 0;
bool registeredWithRoot = false;
unsigned long lastRegistrationAttempt = 0;
unsigned long lastStatusPrint = 0;
unsigned long lastStatusReport = 0;
unsigned long long resetTimer = 0;
volatile uint32_t isrTime[NLIGHTS];
volatile bool buttonEvent[NLIGHTS];

portMUX_TYPE isrMux = portMUX_INITIALIZER_UNLOCKED;

// Task handles
TaskHandle_t buttonTaskHandle = NULL;
TaskHandle_t ledTaskHandle = NULL;
TaskHandle_t statusTaskHandle = NULL;
TaskHandle_t resetTaskHandle = NULL;
TaskHandle_t statusReportTaskHandle = NULL;

// OTA update flag
volatile bool otaInProgress = false;

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
    otaInProgress = true;

    for (int i = 0; i < NLIGHTS; i++) {
        detachInterrupt(buttonPins[i]);
    }

    esp_task_wdt_deinit();  // REQUIRED

    vTaskDelay(pdMS_TO_TICKS(2000));

    performFirmwareUpdate();  // now safe
}

void performFirmwareUpdate() {
    vTaskDelay(1000 / portTICK_PERIOD_MS);  // Allow message to be sent
    Serial.println("[OTA] Starting firmware update...");

    setLedColor(0, 0, 255);  // Blue during update

    vTaskDelay(1000 / portTICK_PERIOD_MS);  // Give time for cleanup

    Serial.println("[OTA] Switching to STA mode...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("[OTA] Connecting to WiFi");
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startTime > WIFI_CONNECT_TIMEOUT) {
            Serial.println("\n[OTA] WiFi connection timeout, restarting...");
            ESP.restart();
            return;
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
        Serial.print(".");
    }
    Serial.println(" connected!");
    Serial.printf("[OTA] IP: %s\n", WiFi.localIP().toString().c_str());

    WiFiClientSecure* client = new WiFiClientSecure();
    if (!client) {
        Serial.println("[OTA] Failed to allocate WiFiClientSecure");
        ESP.restart();
        return;
    }

    client->setInsecure();
    HTTPClient http;
    http.setTimeout(30000);  // 30 second timeout

    Serial.println("[OTA] Connecting to update server...");
    if (!http.begin(*client, firmware_url)) {
        Serial.println("[OTA] Failed to begin HTTP connection");
        delete client;
        ESP.restart();
        return;
    }

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int contentLength = http.getSize();
        Serial.printf("[OTA] Firmware size: %d bytes\n", contentLength);

        if (contentLength <= 0) {
            Serial.println("[OTA] Invalid content length");
            http.end();
            delete client;
            ESP.restart();
            return;
        }

        if (!Update.begin(contentLength)) {
            Serial.printf(
                "[OTA] Not enough space. Required: %d, Available: %d\n",
                contentLength, ESP.getFreeSketchSpace());
            http.end();
            delete client;
            ESP.restart();
            return;
        }

        Serial.println("[OTA] Writing firmware...");
        WiFiClient* stream = http.getStreamPtr();
        size_t written = Update.writeStream(*stream);

        Serial.printf("[OTA] Written %d/%d bytes\n", (int)written,
                      contentLength);

        if (Update.end()) {
            if (Update.isFinished()) {
                Serial.println("[OTA] Update finished successfully!");
                http.end();
                delete client;
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                ESP.restart();
            } else {
                Serial.println("[OTA] Update not finished properly");
                Update.printError(Serial);
            }
        } else {
            Serial.printf("[OTA] Update error: %d\n", Update.getError());
            Update.printError(Serial);
        }
    } else {
        Serial.printf("[OTA] HTTP GET failed, code: %d\n", httpCode);
    }

    http.end();
    delete client;

    Serial.println("[OTA] Update failed, restarting...");
    setLedColor(255, 0, 0);  // Red on failure
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    ESP.restart();
}

void meshInit() {
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);

    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA);
    mesh.onReceive(&receivedCallback);

    deviceId = mesh.getNodeId();
    Serial.printf("SWITCH: Device ID: %u\n", deviceId);
    Serial.printf("SWITCH: Free heap: %d bytes\n", ESP.getFreeHeap());
}

void sendStatusReport(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        lastStatusReport = millis();
        Serial.println("MESH: Sending status report to root");

        JsonDocument doc;
        JsonArray statusArray = doc.to<JsonArray>();

        statusArray.add(String(WiFi.RSSI()));
        statusArray.add(String(millis() / 1000));
        statusArray.add(String(clicks));
        statusArray.add(fw_md5);
        statusArray.add(String(disconnects));

        String message;
        serializeJson(statusArray, message);

        if (mesh.sendSingle(rootId, message)) {
            Serial.println("MESH: Status report sent successfully");
        } else {
            Serial.println("MESH: Failed to send status report");
        }

        vTaskDelay(pdMS_TO_TICKS(STATUS_REPORT_INTERVAL));
    }
}

void receivedCallback(const uint32_t& from, const String& msg) {
    Serial.printf("MESH: [%u] %s\n", from, msg.c_str());

    if (msg == "U") {
        Serial.println("MESH: Firmware update command received");
        setLedColor(0, 0, 255);
        otaInProgress = true;
        return;
    }

    if (msg == "Q") {
        Serial.println("MESH: Registration query received from root");
        rootId = from;
        mesh.sendSingle(rootId, "S");
        Serial.printf("MESH: Sent registration 'S' to root %u\n", rootId);
        return;
    }

    if (msg == "A") {
        Serial.println("MESH: Registration accepted by root");
        registeredWithRoot = true;
        return;
    }

    // Handle other messages if needed
    Serial.printf("MESH: Unknown message from %u: %s\n", from, msg.c_str());
}

void printNodes() {
    auto nodes = mesh.getNodeList();
    Serial.printf("MESH: Connected to %u node(s)\n", nodes.size());
    for (auto node : nodes) {
        Serial.printf("  Node: %u%s\n", node,
                      (node == rootId) ? " (ROOT)" : "");
    }
}

void statusPrint(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        lastStatusPrint = millis();

        Serial.println("\n--- Status Report ---");
        Serial.printf("Device ID: %u\n", deviceId);
        Serial.printf("Firmware MD5: %s\n", fw_md5.c_str());
        Serial.printf("Root ID: %u\n", rootId);
        Serial.printf("Registered: %s\n", registeredWithRoot ? "Yes" : "No");
        Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
        Serial.printf("Uptime: %lu seconds\n", millis() / 1000);
        Serial.printf("WiFi RSSI: %d dBm\n", WiFi.RSSI());
        Serial.printf("Time to reset: %d\n",
                      90 - ((micros() - resetTimer) / 1000000));

        printNodes();
        Serial.println("-------------------\n");

        vTaskDelay(pdMS_TO_TICKS(STATUS_PRINT_INTERVAL));
    }
}

void resetTask(void* pvParameters) {
    while ((micros() - resetTimer) / 1000000 < 90) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (mesh.getNodeList().empty()) {
            registeredWithRoot = false;
        } else if (registeredWithRoot) {
            resetTimer = micros();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP.restart();
}

void IRAM_ATTR buttonISR(void* arg) {
    int index = (intptr_t)arg;
    uint32_t now = micros();
    if (now - lastPress[index] > BUTTON_DEBOUNCE_TIME * 1000) {
        pressed |= (1 << index);
    }
    lastPress[index] = now;
}

void handleButtonsTask(void* pvParameters) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));

        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!pressed) continue;

        for (int i = 0; i < NLIGHTS; i++) {
            if (!(pressed & (1 << i))) continue;

            Serial.printf("RELAY: Button for light %d pressed\n", i);

            pressed &= ~(1 << i);
            clicks++;

            char msg = 'a' + i;
            Serial.printf("BUTTON: Button %d pressed, sending '%c'\n", i, msg);

            if (mesh.getNodeList().empty()) {
                Serial.println("BUTTON: No mesh connection, message not sent");
                setLedColor(255, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            String message(msg);
            if (mesh.sendSingle(rootId, message)) {
                Serial.printf("BUTTON: Sent '%s' to root %u\n", message.c_str(),
                              rootId);
                setLedColor(0, 255, 255);
                vTaskDelay(pdMS_TO_TICKS(50));
            } else {
                Serial.println("BUTTON: Failed to send message");
                setLedColor(255, 128, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }
}

void registerTask(void* pvParameters) {
    while (true) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!registeredWithRoot &&
            millis() - lastRegistrationAttempt >= REGISTRATION_RETRY_INTERVAL) {
            lastRegistrationAttempt = millis();
            Serial.println("MESH: Attempting registration with root...");

            if (rootId == 0) {
                Serial.println("MESH: Root ID unknown, cannot register");
                continue;
            }

            mesh.sendSingle(rootId, "S");
            Serial.printf("MESH: Sent registration 'S' to root %u\n", rootId);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void setup() {
    Serial.begin(115200);
    vTaskDelay(pdMS_TO_TICKS(1000));

    fw_md5 = ESP.getSketchMD5();

    Serial.println("\n\n========================================");
    Serial.println("ESP32-C3 Mesh Switch Node Starting...");
    Serial.printf("Chip Model: %s\n", ESP.getChipModel());
    Serial.printf("Sketch MD5: %s\n", fw_md5.c_str());
    Serial.printf("Chip Revision: %d\n", ESP.getChipRevision());
    Serial.printf("CPU Frequency: %d MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("Flash Size: %d bytes\n", ESP.getFlashChipSize());
    Serial.printf("Reset Timer: %d\n", resetTimer);
    Serial.println("========================================\n");

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

    // Setup new connection callback
    mesh.onNewConnection([](uint32_t nodeId) {
        Serial.printf("MESH: New connection from node %u\n", nodeId);

        // Send registration multiple times to ensure delivery
        vTaskDelay(pdMS_TO_TICKS(1000));  // Wait for connection to stabilize

        if (rootId == 0) {
            Serial.println("MESH: Root ID unknown, cannot register");
            return;
        }

        mesh.sendSingle(rootId, "S");
        Serial.printf("MESH: Sent registration 'S' to root %u\n", rootId);

        printNodes();
    });

    // Setup dropped connection callback
    mesh.onDroppedConnection([](uint32_t nodeId) {
        Serial.printf("MESH: Lost connection to node %u\n", nodeId);

        Serial.println("MESH: Lost connection to root, resetting");
        registeredWithRoot = false;
        disconnects++;
    });

    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(buttonPins[i], INPUT_PULLDOWN);

        attachInterruptArg(buttonPins[i], buttonISR, (void*)i, RISING);
    }

    // Start button handler task
    xTaskCreatePinnedToCore(handleButtonsTask, "ButtonTask", 4096, NULL, 2,
                            &buttonTaskHandle, 0);

    // Start LED status update task
    xTaskCreatePinnedToCore(updateLedStatus, "LedTask", 4096, NULL, 1,
                            &ledTaskHandle, 0);

    // Start status print task
    xTaskCreatePinnedToCore(statusPrint, "StatusPrintTask", 4096, NULL, 1,
                            &statusTaskHandle, 0);

    // Start reset watchdog task
    xTaskCreatePinnedToCore(resetTask, "ResetTask", 4096, NULL, 1,
                            &resetTaskHandle, 0);

    // Start status report task
    xTaskCreatePinnedToCore(sendStatusReport, "SendStatusReportTask", 4096,
                            NULL, 1, &statusReportTaskHandle, 0);

    // Start registration task
    xTaskCreatePinnedToCore(registerTask, "RegisterTask", 4096, NULL, 1, NULL,
                            0);

    Serial.println("SWITCH: Setup complete, waiting for mesh connections...");
}

void loop() {
    static bool otaTaskStarted = false;

    if (otaInProgress && !otaTaskStarted) {
        otaTaskStarted = true;

        Serial.println("[OTA] Disconnecting mesh...");

        mesh.stop();
        vTaskDelay(pdMS_TO_TICKS(2000));

        xTaskCreatePinnedToCore(otaTask, "OTA", 4096, NULL, 5, NULL,
                                0  // Core 0
        );
    }

    if (!otaInProgress) {
        mesh.update();
    } else {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}