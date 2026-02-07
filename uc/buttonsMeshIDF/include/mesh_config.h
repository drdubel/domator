const char* firmware_url =
    "https://turbacz.dry.pl/static/data/switch/firmware.bin";

#ifndef MESH_CONFIG_H
#define MESH_CONFIG_H

/* ── Wi-Fi credentials (router the ROOT connects to) ── */
#define CONFIG_MESH_ROUTER_SSID "rdest"
#define CONFIG_MESH_ROUTER_PASS "3bH9vjxfGAQetv8QlG83QPUItIvwRxHH"

/* ── MQTT broker ── */
#define CONFIG_MQTT_BROKER_URI "mqtt://192.168.3.10:1883"
#define CONFIG_MQTT_USERNAME "mesh_root" /* leave empty if none */
#define CONFIG_MQTT_PASSWORD "nptxVn75zbetksJADRcDSKDW4H2574AH"

/* ── Mesh network parameters ── */
#define CONFIG_MESH_CHANNEL 0 /* 0 = auto */
#define CONFIG_MESH_ID {0x77, 0x77, 0x77, 0x77, 0x77, 0x77}
#define CONFIG_MESH_AP_PASSWD \
    "6fc825b7585a8e8a4cc" /* mesh-internal AP password */
#define CONFIG_MESH_AP_MAX_CONN 6
#define CONFIG_MESH_MAX_LAYER_NUM 6

/* ── MQTT topic prefix ── */
#define MQTT_TOPIC_PREFIX "/switch/state/"

/* ── Mesh data receive buffer ── */
#define MESH_RX_BUF_SIZE 1500

#endif /* MESH_CONFIG_H */