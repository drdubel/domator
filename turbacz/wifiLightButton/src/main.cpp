#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <PubSubClient.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <credentials.h>
#include <painlessMesh.h>

#define NLIGHTS 7
#define LED_PIN 8
#define NUM_LEDS 1

using namespace std;

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

WiFiClient espClient;
PubSubClient client(espClient);

WebServer server(80);

Scheduler userScheduler;
painlessMesh mesh;

const char *upload_html PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 OTA Update</title>
  <meta charset="UTF-8">
  <style>
    body { font-family: Arial; text-align: center; margin: 50px; }
    input[type=file] { margin: 20px; }
    button { padding: 10px 20px; font-size: 16px; cursor: pointer; }
  </style>
</head>
<body>
  <h2>ESP32 OTA Update</h2>
  <form method="POST" action="/update" enctype="multipart/form-data">
    <input type="file" name="firmware">
    <button type="submit">Upload & Update</button>
  </form>
</body>
</html>
)rawliteral";

bool serverStarted = false;
const int buttonPins[NLIGHTS] = {A0, A1, A3, A4, A5, 6, 7};
int lastTimeClick[NLIGHTS];
int debounceDelay = 250;
char whichLight;
int lastButtonState[NLIGHTS] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};

char msg;

void sendMessage();

Task taskSendMessage(TASK_SECOND * 1, TASK_FOREVER, &sendMessage);

void sendMessage() {
    String msg = "1";
    msg += DEVICE_ID;
    mesh.sendBroadcast(msg);
    taskSendMessage.setInterval(random(TASK_SECOND * 1, TASK_SECOND * 5));
}

void receivedCallback(uint32_t from, String &msg) {
    Serial.printf("startHere: Received from %u msg=%s\n", from, msg.c_str());
}

void newConnectionCallback(uint32_t nodeId) {
    Serial.printf("--> startHere: New Connection, nodeId = %u\n", nodeId);
}

void changedConnectionCallback() { Serial.printf("Changed connections\n"); }

void nodeTimeAdjustedCallback(int32_t offset) {
    Serial.printf("Adjusted time %u. Offset = %d\n", mesh.getNodeTime(),
                  offset);
}

void meshSetup() {
    mesh.setDebugMsgTypes(ERROR | STARTUP);

    mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
    mesh.onReceive(&receivedCallback);
    mesh.onNewConnection(&newConnectionCallback);
    mesh.onChangedConnections(&changedConnectionCallback);
    mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);

    userScheduler.addTask(taskSendMessage);
    taskSendMessage.enable();
}

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
    pixels.setPixelColor(0, pixels.Color(r, g, b));
    pixels.show();
}

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

void wifiConnect() {
    Serial.print("Connecting to ");
    Serial.print(ssid);

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");

        setLedColor(255, 0, 0);
        delay(500);
        setLedColor(0, 0, 0);
        delay(500);
    }

    Serial.println(WiFi.localIP());
    Serial.println();
}

void handleRoot() { server.send(200, "text/html", upload_html); }

void handleUpdate() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("Update: %s\n", upload.filename.c_str());
        if (!Update.begin()) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) !=
            upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("Update Success: %u bytes\nRebooting...\n",
                          upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

void serverSetup() {
    server.on("/", HTTP_GET, handleRoot);
    server.on(
        "/update", HTTP_POST,
        []() {
            server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
            ESP.restart();
        },
        handleUpdate);

    server.begin();
    serverStarted = true;
}

void setup() {
    Serial.begin(115200);
    pixels.begin();
    pixels.setBrightness(5);
    setLedColor(0, 0, 0);

    meshSetup();

    for (int i = 0; i < NLIGHTS; i++) {
        pinMode(buttonPins[i], INPUT_PULLDOWN);
    }
}

void loop() {
    for (int i = 0; i < NLIGHTS; i++) {
        int currentState = digitalRead(buttonPins[i]);

        if (millis() - lastTimeClick[i] < debounceDelay) {
            continue;
        }

        if (currentState == HIGH && lastButtonState[i] == LOW) {
            lastTimeClick[i] = millis();

            msg = 'a' + i;
            Serial.print("Publishing message: ");
            Serial.println(msg);
        }

        lastButtonState[i] = currentState;
    }

    updateLedStatus();

    mesh.update();
    server.handleClient();
}
