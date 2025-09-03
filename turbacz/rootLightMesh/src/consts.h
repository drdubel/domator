#include <Arduino.h>

#define HOSTNAME "ROOT_Node"

#define NLIGHTS 7
#define LED_PIN 8
#define NUM_LEDS 1

IPAddress mqtt_broker(192, 168, 3, 244);
const int mqtt_port = 1883;
const char *mqttUser = "root_node";

bool serverStarted = false;
const int buttonPins[NLIGHTS] = {A0, A1, A3, A4, A5, 6, 7};
int lastTimeClick[NLIGHTS];
int debounceDelay = 250;
char whichLight;
int lastButtonState[NLIGHTS] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};

char msg;