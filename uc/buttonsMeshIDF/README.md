# Beetle ESP32-C6 battery switch

Target: **DFRobot Beetle ESP32-C6 V1.1 (DFR1117)**, the small board with
ESP32-C6FH4 and 4 MB flash. Build environment: `esp32c6`. This battery
build requires provisioned `switch_role=remote` and communicates through the
existing ESP-NOW gateway/root; it never joins ESP-MESH itself.

## Wiring

| Button / protocol ID | Board GPIO | Other side of switch |
| --- | --- | --- |
| 1 / a | 4 (A3 / LP_RX) | 3V3 |
| 2 / b | 5 (A4 / LP_TX) | 3V3 |
| 3 / c | 6 (D12 / LP_SDA) | 3V3 |
| 4 / d | 7 (D11 / LP_SCL) | 3V3 |

Internal pulldowns keep unconnected inputs low; unused buttons need no wiring.
These are raw GPIO numbers, not Arduino digital aliases. GPIO0 is connected to
the battery voltage divider, GPIO9 is BOOT, GPIO12/13 are USB, and GPIO15 is
the active-low onboard LED. Leave the button pins free of other peripherals
and JTAG probes. The larger FireBeetle 2 DFR1075 has a different exposed pinout.

## Battery connection

Connect **one protected 3.7 V nominal Li-ion/LiPo cell (4.2 V fully charged)**:
positive to **BAT**, negative to **GND**. A protected 18650 cell is also suitable
if its charging/current ratings match. Never put the cell on 3V3 or VIN, and
do not use a two-cell pack, primary/non-rechargeable battery, or LiFePO4 cell
with the onboard charger. Check polarity before connecting.

V1.1 uses a **TP4057 charger** (USB powered, up to 500 mA) and a
**TPS62A02 buck regulator**. Choose a cell rated for the board's charging
current and ESP32 radio current pulses. The schematic does not include a
separate cell discharge-protection circuit: use a protected cell/pack or add
protection for over-discharge, short circuits, and over-current. Charging
management alone does not provide those functions.

DFRobot specifies **37 microamps deep-sleep current for V1.1**. That is a board
specification, not a measurement of this firmware or average consumption.
Heartbeats, radio retries, button activity and held inputs add consumption.
Measure at BAT with USB disconnected; USB power/debugging and external loads
can change the result. An asserted button draws current through its internal
pulldown, so an indefinitely closed stateful switch costs more than an idle
momentary button.

## Build, flash and provision

Run from this directory with PlatformIO installed:

```sh
pio run -e esp32c6
pio run -e esp32c6 -t upload --upload-port /dev/cu.usbmodemXXX
```

Use the actual USB port and close any serial monitor before uploading or
provisioning. The firmware stays awake while connected to a USB host. If it
was already asleep when the USB cable was connected, press RST to reconnect.
If an older firmware disconnects USB immediately, hold **BOOT**, press/release
**RST**, then release **BOOT** to enter the ROM downloader. Run provisioning
while in the downloader; installing the fix first is not necessary. On macOS,
use `ls /dev/cu.usbmodem*` to find the port, which may change after resetting.
First upload writes the bootloader and current partition table as well as the
application. Then follow [provisioning setup](provisioning/README.md#setup),
retain the installation credentials, and add these rows to a private CSV:

```csv
switch_role,data,string,remote
gateway_mac,data,string,AA:BB:CC:DD:EE:FF
remote_channel,data,u8,11
remote_key,data,string,<same 64 hex digits as gateway and all possible roots>
```

`gateway_mac` is the enabled gateway's **STA** MAC from RCM, and the channel
must match its actual mesh radio channel. Provision and reset:

```sh
./provisioning/provision.sh -C esp32c6 -p /dev/cu.usbmodemXXX -c /path/to/private.csv
```

A missing/invalid provisioning record leaves USB available while connected
to a computer. Provision by USB and reset to load the new credentials. After
30 seconds without a USB host, the unprovisioned board sleeps until reset to
avoid draining a disconnected battery. A USB charger/power bank alone does
not count as a computer connection.
Updated Turbacz registers the Beetle with four buttons; map a-d as usual.
With an older backend, set the device's button count to **4** in RCM after
its first heartbeat.

For OTA, update Turbacz and use its **C6 Remote FW** upload button with
`.pio/build/esp32c6/firmware.bin`. Provision `ota_url` with the authenticated
`https://<your-server>/firmware/beetle-esp32c6.bin` endpoint and the matching
`ota_token`. This slot is separate from C3 `switch.bin` and ESP32 `relay.bin`.
Until a C6 image is available, omit `ota_url`. OTA requests are picked up on
the next transmission/heartbeat; the double-press/hold gesture is also retained.

## Power behavior

- A USB host connection keeps the CPU awake and USB accessible for programming
  and provisioning. Buttons and heartbeats continue to work. Normal power-on
  and reset allow three seconds for USB enumeration; deep-sleep battery wakes
  do not add that delay. After USB disconnection, sleep resumes after a short
  grace period. Measure battery sleep current with USB disconnected.
- Stable inputs enter **deep sleep** and wake on any GPIO4-7 level change or
  the existing 60-second heartbeat. Each pin has its own opposite-level wake
  trigger, so held buttons do not cause repeated boots and releases wake too.
- Debounce waits and a 250 ms interaction window after edges use **light
  sleep**. This keeps short taps and double presses from paying for additional
  cold boots. The existing 50 ms debounce remains in place.
- RTC RAM retains button levels, press/release times, OTA gesture state,
  heartbeat deadline, click count, uptime base, message session/sequence and
  command duplicate history. RTC-backed time includes time spent asleep.
  Button activity does not write flash/NVS.
- The Wi-Fi stack starts only when needed, uses RAM settings, and stops before
  sleep. ESP-NOW uses the fixed provisioned channel: no discovery scan, Wi-Fi
  association, MQTT connection, or mesh maintenance on the battery device.
- The LED is held off during sleep. Routine boot/application logging is reduced.
  Radio ACK waits block on the receive queue; retries remain bounded at three
  45 ms windows. The one-minute heartbeat preserves the backend's 150-second
  remote timeout and queued OTA delivery.
- ESP32-C3 builds retain their seven GPIOs and light-sleep lifecycle.

Deep sleep adds a cold boot before debounce/radio work. A pulse that ends before
the GPIOs are sampled after boot can be missed; this is a human-operated switch,
not a pulse counter. Minimum tap duration, press/release latency, long-held
switches, simultaneous buttons, the OTA gesture, failed ACKs, gateway outages,
and actual average current must be checked on hardware. No sub-200 ms latency
or battery-life figure has been measured for this implementation.

References: [DFRobot board documentation](https://wiki.dfrobot.com/dfr1117/),
[official V1.0/V1.1 schematics](https://dfimg.dfrobot.com/wiki/19405/DFR1117_beetle-esp32-c6_schematics_V1.1.zip),
[ESP-IDF C6 sleep documentation](https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32c6/api-reference/system/sleep_modes.html).
