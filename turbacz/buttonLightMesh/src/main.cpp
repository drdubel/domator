#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <credentials.h>
#include <painlessMesh.h>

// Pin and hardware definitions
#define NLIGHTS 7
#define LED_PIN 8
#define NUM_LEDS 1

// Timing constants
#define DEBOUNCE_DELAY 250
#define STATUS_PRINT_INTERVAL 30000
#define WIFI_CONNECT_TIMEOUT 20000
#define REGISTRATION_RETRY_INTERVAL 10000

// Function declarations
void receivedCallback(const uint32_t& from, const String& msg);
void meshInit();
void performFirmwareUpdate();
void setLedColor(uint8_t r, uint8_t g, uint8_t b);
void updateLedStatus();
void printNodes();
void sendRegistration();

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

painlessMesh mesh;

// Root node ID - will be discovered dynamically
uint32_t rootId = 0;
uint32_t deviceId = 0;

const int buttonPins[NLIGHTS] = {A0, A1, A3, A4, A5, 6, 7};
unsigned long lastTimeClick[NLIGHTS] = {0};
int lastButtonState[NLIGHTS] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
bool registeredWithRoot = false;
unsigned long lastRegistrationAttempt = 0;
unsigned long lastStatusPrint = 0;

const char* firmware_url =
    "https://czupel.dry.pl/static/data/switch/firmware.bin";

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
    pixels.setPixelColor(0, pixels.Color(r, g, b));
    pixels.show();
}

void updateLedStatus() {
    // Get mesh connection status
    bool meshConnected = mesh.getNodeList().size() > 0;

    if (meshConnected && registeredWithRoot) {
        setLedColor(0, 255, 0);  // Green - fully connected
    } else if (meshConnected) {
        setLedColor(255, 255,
                    0);  // Yellow - mesh connected, waiting for registration
    } else {
        setLedColor(255, 0, 0);  // Red - not connected
    }
}

void performFirmwareUpdate() {
    Serial.println("[OTA] Starting firmware update...");
    setLedColor(0, 0, 255);  // Blue during update

    Serial.println("[OTA] Stopping mesh...");
    mesh.stop();

    delay(1000);  // Give time for cleanup

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
        delay(500);
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
                delay(1000);
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
    delay(2000);
    ESP.restart();
}

void meshInit() {
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);

    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA, 6, 0, 20);
    mesh.onReceive(&receivedCallback);

    deviceId = mesh.getNodeId();
    Serial.printf("SWITCH: Device ID: %u\n", deviceId);
    Serial.printf("SWITCH: Free heap: %d bytes\n", ESP.getFreeHeap());
}

void receivedCallback(const uint32_t& from, const String& msg) {
    Serial.printf("MESH: [%u] %s\n", from, msg.c_str());

    if (msg == "U") {
        Serial.println("MESH: Firmware update command received");
        setLedColor(0, 0, 255);
        performFirmwareUpdate();
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

void sendRegistration() {
    auto nodes = mesh.getNodeList();

    if (nodes.empty()) {
        Serial.println("MESH: No nodes connected, cannot register");
        registeredWithRoot = false;
        return;
    }

    // Always broadcast registration to ensure root receives it
    Serial.println("MESH: Broadcasting registration 'S' to all nodes");
    mesh.sendBroadcast("S");

    // If we have a known root ID, also send directly
    if (rootId != 0) {
        Serial.printf("MESH: Also sending 'S' directly to root %u\n", rootId);
        mesh.sendSingle(rootId, "S");
    } else {
        // Try to identify root - it's usually the node with lowest ID or first
        // in list
        rootId = nodes.front();
        Serial.printf("MESH: Assuming node %u as root\n", rootId);
        delay(500);  // Give time for mesh to stabilize
        mesh.sendSingle(rootId, "S");
    }

    registeredWithRoot = true;
    Serial.println("MESH: Registration sent");

    updateLedStatus();
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n\n========================================");
    Serial.println("ESP32-C3 Mesh Switch Node Starting...");
    Serial.printf("Chip Model: %s\n", ESP.getChipModel());
    Serial.printf("Chip Revision: %d\n", ESP.getChipRevision());
    Serial.printf("CPU Frequency: %d MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("Flash Size: %d bytes\n", ESP.getFlashChipSize());
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

        // Update root ID if not set
        if (rootId == 0) {
            rootId = nodeId;
            Serial.printf("MESH: Setting root ID to %u\n", rootId);
        }

        // Send registration multiple times to ensure delivery
        delay(1000);  // Wait for connection to stabilize
        for (int i = 0; i < 3; i++) {
            mesh.sendBroadcast("S");
            Serial.printf("MESH: Sent registration 'S' (attempt %d/3)\n",
                          i + 1);
            delay(500);
        }

        registeredWithRoot = true;
        updateLedStatus();

        printNodes();
    });

    // Setup dropped connection callback
    mesh.onDroppedConnection([](uint32_t nodeId) {
        Serial.printf("MESH: Lost connection to node %u\n", nodeId);

        // If we lost connection to root, reset registration
        if (nodeId == rootId) {
            Serial.println("MESH: Lost connection to root, resetting");
            registeredWithRoot = false;
            rootId = 0;
        }

        updateLedStatus();
    });

    Serial.println("SWITCH: Setup complete, waiting for mesh connections...");
}

void loop() {
    mesh.update();

    unsigned long currentMillis = millis();

    // Periodic registration retry if not registered
    if (!registeredWithRoot &&
        currentMillis - lastRegistrationAttempt > REGISTRATION_RETRY_INTERVAL) {
        lastRegistrationAttempt = currentMillis;

        auto nodes = mesh.getNodeList();
        if (!nodes.empty()) {
            Serial.println("MESH: Retrying registration (broadcasting 'S')...");
            mesh.sendBroadcast("S");

            if (rootId != 0) {
                mesh.sendSingle(rootId, "S");
            }
        }
    }

    // Update LED status periodically
    static unsigned long lastLedUpdate = 0;
    if (currentMillis - lastLedUpdate > 5000) {
        lastLedUpdate = currentMillis;
        updateLedStatus();
    }

    // Handle button presses
    for (int i = 0; i < NLIGHTS; i++) {
        int currentState = digitalRead(buttonPins[i]);

        // Skip if within debounce period
        if (currentMillis - lastTimeClick[i] < DEBOUNCE_DELAY) {
            continue;
        }

        // Detect button press (LOW to HIGH transition)
        if (currentState == HIGH && lastButtonState[i] == LOW) {
            lastTimeClick[i] = currentMillis;

            char msg = 'a' + i;
            Serial.printf("BUTTON: Button %d pressed, sending '%c'\n", i, msg);

            // Check if we're connected to mesh
            if (mesh.getNodeList().empty()) {
                Serial.println("BUTTON: No mesh connection, message not sent");
                setLedColor(255, 0, 0);  // Flash red
                delay(100);
                updateLedStatus();
                continue;
            }

            // Send to root if known, otherwise broadcast
            if (rootId != 0) {
                String message =
                    String(msg);  // Format: just the letter (e.g., 'a')
                if (mesh.sendSingle(rootId, message)) {
                    Serial.printf("BUTTON: Sent '%s' to root %u\n",
                                  message.c_str(), rootId);

                    // Flash LED to confirm
                    setLedColor(0, 255, 255);  // Cyan flash
                    delay(50);
                    updateLedStatus();
                } else {
                    Serial.println("BUTTON: Failed to send message");
                    setLedColor(255, 128, 0);  // Orange flash
                    delay(100);
                    updateLedStatus();
                }
            } else {
                String message = String(msg);
                mesh.sendBroadcast(message);
                Serial.printf("BUTTON: Broadcast '%s'\n", message.c_str());
            }
        }

        lastButtonState[i] = currentState;
    }

    // Periodic status report
    if (currentMillis - lastStatusPrint >= STATUS_PRINT_INTERVAL) {
        lastStatusPrint = currentMillis;

        Serial.println("\n--- Status Report ---");
        Serial.printf("Device ID: %u\n", deviceId);
        Serial.printf("Root ID: %u\n", rootId);
        Serial.printf("Registered: %s\n", registeredWithRoot ? "Yes" : "No");
        Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
        Serial.printf("Uptime: %lu seconds\n", currentMillis / 1000);

        printNodes();
        Serial.println("-------------------\n");
    }
}