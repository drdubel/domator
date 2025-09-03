#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <consts.h>
#include <credentials.h>
#include <painlessMesh.h>
#include <webpage.h>

#include <map>

void receivedCallback(const uint32_t& from, const String& msg);
void newConnectionCallback(uint32_t nodeId);

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

IPAddress myIP(0, 0, 0, 0);
AsyncWebServer server(80);
painlessMesh mesh;
WiFiClient wifiClient;
PubSubClient mqttClient(mqtt_broker, mqtt_port, wifiClient);

IPAddress getlocalIP() { return IPAddress(mesh.getStationIP()); }

std::map<uint32_t, String> nodes;

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
    pixels.setPixelColor(0, pixels.Color(r, g, b));
    pixels.show();
}

// DO NAPRAWY
void updateLedStatus() {
    bool wifi_ok = (WiFi.status() == WL_CONNECTED);
    bool server_ok = serverStarted;

    if (wifi_ok && server_ok)
        setLedColor(255, 255, 255);
    else if (wifi_ok)
        setLedColor(255, 0, 0);
    else if (server_ok)
        setLedColor(0, 255, 0);
    else
        setLedColor(0, 0, 0);
}

void mqttConnect() {
    Serial.print("Connecting to MQTT broker at ");
    Serial.print(mqtt_broker);
    Serial.print(" with user ");
    Serial.println(mqttUser);

    while (!mqttClient.connected()) {
        if (mqttClient.connect(mqttUser)) {
            Serial.println("Connected to MQTT broker");
        } else {
            setLedColor(0, 255, 0);
            delay(500);
            setLedColor(0, 0, 0);
            delay(500);
        }
    }

    mqttClient.publish("/switch/0", "connected");
}

void serverInit() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/html", webpage);
    });
    server.on(
        "/update", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            bool updateSuccess = !Update.hasError();
            request->send(200, "text/plain",
                          updateSuccess ? "Update successful. Rebooting..."
                                        : "Update failed!");
            if (updateSuccess) {
                delay(1000);
                ESP.restart();
            }
        },
        [](AsyncWebServerRequest* request, String filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            if (!index) {
                Update.begin(UPDATE_SIZE_UNKNOWN);
            }
            if (Update.write(data, len) != len) {
                Update.printError(Serial);
            }
            if (final) {
                if (Update.end(true)) {
                    Serial.println("Update complete");
                } else {
                    Update.printError(Serial);
                }
            }
        });
    server.begin();

    Serial.println("HTTP server started");
}

void meshInit() {
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);

    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA, 6);
    mesh.onReceive(&receivedCallback);
    mesh.onNewConnection(&newConnectionCallback);

    mesh.stationManual(STATION_SSID, STATION_PASSWORD);
    mesh.setHostname(HOSTNAME);

    mesh.setRoot(true);
    mesh.setContainsRoot(true);

    uint32_t rootId = mesh.getNodeId();
    Serial.println("ROOT:" + String(rootId));

    while (WiFi.status() != WL_CONNECTED) {
        mesh.update();
        Serial.print(".");

        delay(250);
        setLedColor(255, 0, 0);
        delay(250);
        setLedColor(0, 0, 0);
    }

    myIP = getlocalIP();
    Serial.println("My IP is " + myIP.toString());

    serverInit();
    mqttConnect();
}

void newConnectionCallback(uint32_t nodeId) {
    uint32_t rootId = mesh.getNodeId();

    mesh.sendSingle(nodeId, String(rootId));
}

void receivedCallback(const uint32_t& from, const String& msg) {
    Serial.printf("bridge: Received from %u msg=%s\n", from, msg.c_str());

    if (msg[0] >= 97) {
        if (WiFi.status() != WL_CONNECTED) return;
        if (!mqttClient.connected()) mqttConnect();

        String topic = "/switch/" + nodes[from];

        Serial.println("Publishing to topic: " + topic);
        mqttClient.publish(topic.c_str(), msg.c_str());
    } else {
        nodes[from] = msg;
        Serial.print("Node ");
        Serial.print(from);
        Serial.print(" registered as ");
        Serial.println(msg);
    }
}

void setup() {
    Serial.begin(115200);

    pixels.begin();
    pixels.setBrightness(5);
    setLedColor(0, 0, 0);

    meshInit();
}

void loop() {
    mesh.update();

    if (WiFi.status() == WL_CONNECTED) {
        if (!mqttClient.connected()) mqttConnect();

        mqttClient.loop();
    }
}