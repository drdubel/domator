#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <credentials.h>
#include <painlessMesh.h>

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

// Function declarations
void receivedCallback(const uint32_t& from, const String& msg);
void meshInit();
void performFirmwareUpdate();
void printStatus();
void syncLightStates(uint32_t targetNodeId);

painlessMesh mesh;

uint32_t rootId = 0;
uint32_t deviceId = 0;
uint32_t disconnects = 0;
uint32_t clicks = 0;

const int relays[NLIGHTS] = {32, 33, 25, 26, 27, 14, 12, 13};
const int buttons[NLIGHTS] = {2, 15, 4, 0, 17, 16, 18, 5};
int lights[NLIGHTS] = {0, 0, 0, 0, 0, 0, 0, 0};
volatile bool buttonState[NLIGHTS] = {0, 0, 0, 0, 0, 0, 0, 0};
volatile uint32_t lastPress[NLIGHTS] = {0, 0, 0, 0, 0, 0, 0, 0};
volatile uint8_t pressed = 0;

bool registeredWithRoot = false;
unsigned long lastRegistrationAttempt = 0;
unsigned long lastStatusPrint = 0;
unsigned long lastStatusReport = 0;
unsigned long long resetTimer = 0;
String fw_md5;  // MD5 of the firmware as flashed

void performFirmwareUpdate() {
    vTaskDelay(1000 / portTICK_PERIOD_MS);  // Allow message to be sent
    Serial.println("[OTA] Starting firmware update...");

    Serial.println("[OTA] Stopping mesh...");
    mesh.stop();

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
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP.restart();
}

void meshInit() {
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);

    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA);
    mesh.onReceive(&receivedCallback);

    deviceId = mesh.getNodeId();
    Serial.printf("RELAY: Device ID: %u\n", deviceId);
    Serial.printf("RELAY: Free heap: %d bytes\n", ESP.getFreeHeap());
}

void sendStatusReport() {
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
}

void syncLightStates(uint32_t targetNodeId) {
    Serial.printf("RELAY: Syncing all light states to node %u\n", targetNodeId);

    for (int i = 0; i < NLIGHTS; i++) {
        char message[2];
        message[0] = 'A' + i;
        message[1] = lights[i] ? '1' : '0';

        if (mesh.sendSingle(targetNodeId, String(message))) {
            Serial.printf("RELAY: Sent state %s to node %u\n", message,
                          targetNodeId);
        } else {
            Serial.printf("RELAY: Failed to send state %s to node %u\n",
                          message, targetNodeId);
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);  // Small delay between messages to
                                              // prevent flooding
    }
}

void receivedCallback(const uint32_t& from, const String& msg) {
    Serial.printf("MESH: [%u] %s\n", from, msg.c_str());

    // Handle sync request from switch nodes
    if (msg == "S") {
        Serial.printf("MESH: Switch node %u requesting state sync\n", from);
        syncLightStates(from);
        return;
    }

    // Handle registration query from switch nodes
    if (msg == "Q") {
        Serial.println("MESH: Registration query received from root");
        rootId = from;
        mesh.sendSingle(rootId, "R");
        Serial.printf("MESH: Sent registration 'R' to root %u\n", rootId);
        return;
    }

    // Handle firmware update command
    if (msg == "U") {
        Serial.println("MESH: Firmware update command received");
        performFirmwareUpdate();
        return;
    }

    if (msg == "P") {
        Serial.printf("MESH: Received ping from %u, sending pong\n", from);
        mesh.sendSingle(from, "P");
        return;
    }

    // Handle light control messages (e.g., "a0", "b1", etc.)
    if (msg.length() == 2 && msg[0] >= 'a' && msg[0] < 'a' + NLIGHTS) {
        int lightIndex = msg[0] - 'a';
        int newState = msg[1] - '0';

        if (newState != 0 && newState != 1) {
            Serial.printf("MESH: Invalid state '%c' in message from %u\n",
                          msg[1], from);
            return;
        }

        lights[lightIndex] = newState;
        clicks++;
        digitalWrite(relays[lightIndex], newState ? HIGH : LOW);

        Serial.printf("RELAY: Light %c set to %s by node %u\n",
                      'a' + lightIndex, newState ? "ON" : "OFF", from);

        // Send confirmation back
        char response[3];
        response[0] = 'A' + lightIndex;
        response[1] = newState ? '1' : '0';
        response[2] = '\0';

        if (mesh.sendSingle(from, String(response))) {
            Serial.printf("RELAY: Sent confirmation %s to node %u\n", response,
                          from);
        }
        return;
    }

    // Handle light control messages (e.g., "a", "b", etc.)
    if (msg.length() == 1 && msg[0] >= 'a' && msg[0] < 'a' + NLIGHTS) {
        int lightIndex = msg[0] - 'a';
        lights[lightIndex] = !lights[lightIndex];
        clicks++;
        digitalWrite(relays[lightIndex], lights[lightIndex] ? HIGH : LOW);

        Serial.printf("RELAY: Light %c toggled to %s by node %u\n",
                      'a' + lightIndex, lights[lightIndex] ? "ON" : "OFF",
                      from);

        // Send confirmation back
        char response[3];
        response[0] = 'A' + lightIndex;
        response[1] = lights[lightIndex] ? '1' : '0';
        response[2] = '\0';

        if (mesh.sendSingle(from, String(response))) {
            Serial.printf("RELAY: Sent confirmation %s to node %u\n", response,
                          from);
        }
        return;
    }

    if (msg == "A") {
        Serial.println("MESH: Registration accepted by root");
        registeredWithRoot = true;
        return;
    }

    // Unknown message - just log it, don't respond
    Serial.printf("MESH: Unknown/unhandled message '%s' from %u\n", msg.c_str(),
                  from);
}

void statusPrint() {
    lastStatusPrint = millis();

    Serial.println("\n--- Status Report ---");
    Serial.printf("Device ID: %u\n", deviceId);
    Serial.printf("Root ID: %u\n", rootId);
    Serial.printf("Registered: %s\n", registeredWithRoot ? "Yes" : "No");
    Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("Uptime: %lu seconds\n", millis() / 1000);
    Serial.printf("Sketch MD5: %s\n", fw_md5.c_str());

    Serial.println("\nRelay States:");
    for (int i = 0; i < NLIGHTS; i++) {
        Serial.printf("  Light %c (Pin %d): %s\n", 'a' + i, relays[i],
                      lights[i] ? "ON" : "OFF");
    }

    auto nodes = mesh.getNodeList();
    Serial.printf("\nMesh Network: %u node(s)\n", nodes.size());
    for (auto node : nodes) {
        Serial.printf("  Node: %u%s\n", node,
                      (node == rootId) ? " (ROOT)" : "");
    }
    Serial.println("-------------------\n");
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

void setup() {
    Serial.begin(115200);
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    fw_md5 = ESP.getSketchMD5();

    Serial.println("\n\n========================================");
    Serial.println("ESP32 Mesh Relay Node Starting...");
    Serial.printf("Chip Model: %s\n", ESP.getChipModel());
    Serial.printf("Sketch MD5: %s\n", fw_md5.c_str());
    Serial.printf("Chip Revision: %d\n", ESP.getChipRevision());
    Serial.printf("CPU Frequency: %d MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("Flash Size: %d bytes\n", ESP.getFlashChipSize());
    Serial.println("========================================\n");

    // Initialize relay pins
    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(relays[i], OUTPUT);
        digitalWrite(relays[i], LOW);
        Serial.printf("RELAY: Initialized relay %d (Pin %d)\n", i, relays[i]);
    }

    // Initialize additional pin
    pinMode(23, OUTPUT);
    digitalWrite(23, LOW);

    // Initialize mesh
    meshInit();

    // Setup new connection callback
    mesh.onNewConnection([](uint32_t nodeId) {
        Serial.printf("MESH: New connection from node %u\n", nodeId);

        // Update root ID if not set
        vTaskDelay(1000 /
                   portTICK_PERIOD_MS);  // Wait for connection to stabilize

        if (rootId == 0) {
            Serial.println("MESH: Root ID unknown, cannot register");
            return;
        }

        mesh.sendSingle(rootId, "R");
        Serial.printf("MESH: Sent registration 'R' to root %u\n", rootId);
    });

    // Setup dropped connection callback
    mesh.onDroppedConnection([](uint32_t nodeId) {
        Serial.printf("MESH: Lost connection to node %u\n", nodeId);

        Serial.println("MESH: Lost connection to root, resetting");
        disconnects++;
        registeredWithRoot = false;
    });

    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(buttons[i], INPUT_PULLDOWN);

        // Use attachInterruptArg to pass the index to the ISR
        attachInterruptArg(digitalPinToInterrupt(buttons[i]), buttonISR,
                           (void*)i, CHANGE);
    }

    Serial.println("RELAY: Setup complete, waiting for mesh connections...");
}

void loop() {
    mesh.update();

    // Handle button presses
    if (pressed) {
        for (int i = 0; i < NLIGHTS; i++) {
            if (pressed & (1 << i)) {
                Serial.printf("RELAY: Button for light %d pressed\n", i);
                String msg =
                    String(char('a' + i)) + String(char(buttonState[i] + '0'));
                mesh.sendSingle(rootId, msg);

                pressed &= ~(1 << i);
            }
        }
    }

    // Periodic status print
    if (millis() - lastStatusPrint >= STATUS_PRINT_INTERVAL) {
        statusPrint();

        if (mesh.getNodeList().empty()) {
            registeredWithRoot = false;
        } else {
            resetTimer = millis();
        }
    }

    if (!registeredWithRoot &&
        millis() - lastRegistrationAttempt >= REGISTRATION_RETRY_INTERVAL) {
        lastRegistrationAttempt = millis();
        Serial.println("MESH: Attempting registration with root...");

        if (rootId == 0) {
            Serial.println("MESH: Root ID unknown, cannot register");
            return;
        }

        mesh.sendSingle(rootId, "R");
        Serial.printf("MESH: Sent registration 'R' to root %u\n", rootId);
    }

    if (millis() - lastStatusReport >= STATUS_REPORT_INTERVAL &&
        registeredWithRoot) {
        sendStatusReport();
    }
}
