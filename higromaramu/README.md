# Higromaramu

Firmware project for the hygrometer/pressure microcontroller node.

## Structure

- `platformio.ini` — PlatformIO project configuration
- `src/main.cpp` — firmware entrypoint

## Build and flash

From this directory:

```bash
pio run
pio run -t upload
pio device monitor
```
