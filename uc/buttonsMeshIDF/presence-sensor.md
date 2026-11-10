# ESP32-C3 SuperMini presence sensor

Target: an **ESP32-C3 SuperMini** with a **Waveshare HMMD mmWave sensor**
(SKU 26536, ICLegend S3KM1110, 24 GHz FMCW). The node is a powered mesh member
like a normal switch: it relays for other nodes and can become root. It reports
presence on three inputs that RCM connects to relay outputs like switch
buttons, and RCM edits the radar's range, delay and sensitivity.

## Wiring

| HMMD pin | SuperMini | Notes |
| --- | --- | --- |
| 3V3 | 3.3 | 3.0–3.6 V, about 50 mA average |
| GND | G | |
| TX | GPIO5 | radar to C3 (UART1 RX, pulled up) |
| RX | GPIO4 | C3 to radar (UART1 TX) |
| OT2 | not connected | presence is read over UART |

Power the SuperMini through USB-C. GPIO20/21 stay free: the C3 console uses
UART0 there and mirrors it on USB. The SuperMini's active-low blue LED (GPIO8)
can show presence; it is off until enabled in RCM.

Mount the sensor as Waveshare recommends: on the ceiling at 2.7–3 m, or on a
wall at 1.5–2 m with the pin header at the top. Keep fans, curtains and other
moving objects out of its field of view.

## Build, flash and provision

The sensor runs the normal `esp32c3` image, the same `switch.bin` as C3
switches. Run from this directory:

```sh
pio run -e esp32c3 -t upload --upload-port /dev/cu.usbmodemXXX
```

Provision the installation credentials as for any node (see
[provisioning](provisioning/README.md)) and add the role:

```csv
switch_role,data,string,presence
```

```sh
./provisioning/provision.sh -C esp32c3 -p /dev/cu.usbmodemXXX -c /path/to/private.csv
```

Keep `ota_url` pointing at `/firmware/switch.bin`: **Switch FW** uploads and
the **Switches** OTA button update presence sensors together with C3 switches.

## RCM

After its first status report the sensor appears as a switch card with an
**mmWave** badge, its occupancy and target distance, and three inputs.
Connect them to relay outputs like switch buttons:

| Input | Connected outputs |
| --- | --- |
| Presence: on / off | turn on while someone is present, off once the area is clear |
| Arrival: on | turn on when presence is detected; the sensor never turns them off |
| Departure: off | turn off once the area is clear; the sensor never turns them on |

The mesh root routes these events itself, as it does for buttons, so lights
keep following presence while Turbacz is unreachable. Update every
root-capable node to this firmware: an older root toggles connected outputs
on each presence change instead. Home Assistant receives the inputs as event
entities: `press` when presence starts, `release` when the area is clear.

**⚙ Sensor** opens the radar settings:

- **Detection range**: nearest and farthest range gate. Each gate covers
  0.7 m (gate 12 ≈ 8.4 m); movement outside the range is ignored.
- **Clear after**: seconds without detection before the area is reported
  clear (0–65535).
- **Indicator LED**: light the SuperMini's LED while presence is detected.
- **Sensitivity per gate**: the trigger (start) and hold (keep) threshold of
  each gate, in dB like Waveshare's tool. Lower values are more sensitive;
  **More/Less sensitive** shift every threshold by 1 dB.
- **Live energy** streams each gate's energy once a second, drawn against its
  thresholds, while the dialog stays open. Tune a gate until a person raises
  it above the trigger threshold and the empty area stays below the hold one.
- **Calibrate** gives you 10 seconds to leave, measures the empty area for 20
  seconds, and sets each gate's trigger threshold 5 dB and hold threshold
  3.5 dB above the highest energy seen (the margins of Waveshare's tool).
- **Defaults** restores the defaults of Waveshare's tool: gates 0–12, 30 s
  and its per-gate thresholds.
- **Reload** reads the settings stored in the radar.

Settings live in the radar's own flash. After each change the sensor reads
every value back and reports it; RCM shows those values, and an error when no
report arrives within 15 seconds (calibration: after the measurement). The
sensor also reports its settings after starting, once it has joined the mesh;
while Turbacz has none stored, it asks the sensor again every 5 minutes.

## Behaviour

- After a restart the first state is sent once it has lasted the absence
  delay, and at least 60 seconds. A person sitting still is not reported as
  absent just because the sensor restarted, and a change missed during the
  restart is still corrected.
- Changes made while the mesh is disconnected are sent after it reconnects;
  only the latest state is sent.
- When the radar stops reporting for 3 seconds the card shows *Radar not
  responding*, and the node reinitialises the radar every 5 seconds.

## Protocol

The radar talks at 115200 baud, 8N1, little endian. Commands are
`FD FC FB FA | length | command | data | 04 03 02 01`; replies set 0x0100 in
the command and start with a status word (0 = success). Enabling
configuration mode (`0x00FF`, protocol 2) reports a 32-byte buffer, so a frame
carries at most three parameter writes (`0x0007`) or four reads (`0x0008`).
Leaving configuration mode (`0x00FE`) stores the values in the radar's flash.

| Parameter | Meaning |
| --- | --- |
| `0x0000` | nearest gate, 0–15 |
| `0x0001` | farthest gate, 0–15 |
| `0x0004` | absence delay, seconds |
| `0x0010 + gate` | trigger threshold (raw energy) |
| `0x0020 + gate` | hold threshold (raw energy) |

The node selects report mode (`0x0012`, value 4). The radar then sends
`F4 F3 F2 F1 | 35 | presence | distance (cm) | 16 gate energies | F8 F7 F6 F5`
ten times a second. This is the protocol of the HLK-LD2420, which uses the
same chip; [src/radar_protocol.h](src/radar_protocol.h) is the parser, tested
on the host by [tests/radar_protocol_test.c](tests/radar_protocol_test.c).

Turbacz sends `{"radar": "read" | "apply" | "factory" | "calibrate" | "live"}`
commands to `/switch/cmd/<id>`. The sensor answers with
`{"type": "presence", "report": "config" | "live", ...}` status messages, which
the root publishes on `/switch/state/root`. Its regular status reports also
carry `presence`, `distance` and `radar`.

Check detection range, calibration and the root's on/off routing on the real
hardware before relying on the sensor; this firmware has only been verified on
the host and by building the image.
