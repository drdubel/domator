# UC firmware projects

This directory contains microcontroller firmware projects used by Domator.

## Projects

- `blind_mover` — STM32 blind mover controller (PlatformIO)
- `blinds_wifi` — ESP8266 Wi-Fi bridge for blinds (PlatformIO)
- `buttonsMeshIDF` — ESP-IDF/PlatformIO mesh-based buttons/relay/root firmware
- `heating` — ESP8266 heating controller (PlatformIO)

Each subdirectory is an independent firmware project with its own `platformio.ini`.

## Credentials

`buttonsMeshIDF` keeps no WiFi, mesh, MQTT or OTA settings in the firmware
image. They live in a dedicated `creds` NVS partition and are written over the
cable at programming time — see
[buttonsMeshIDF/provisioning/README.md](buttonsMeshIDF/provisioning/README.md).
A binary of that firmware therefore discloses nothing, and one build runs on
every installation.

The ESP8266 projects (`blinds_wifi`, `heating`) still `#include "credentials.h"`
and compile their secrets in. That file is gitignored, but the resulting
binaries contain the WiFi and MQTT passwords in the clear.
