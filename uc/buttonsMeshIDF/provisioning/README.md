# Provisioning

The firmware contains no WiFi, mesh, MQTT or OTA settings. They live in the
`creds` NVS partition and are written over the cable when a device is
programmed.

## Why

Settings used to be `CONFIG_*` options in `menuconfig`, which meant they were
compiled into the image as plain strings. `strings firmware.bin` recovered the
WiFi password, the mesh AP password and the broker credentials — which is
exactly what happened when an OTA image turned out to be downloadable without
authentication.

Two things follow from moving them out:

- A leaked or shared binary discloses nothing.
- The build no longer depends on the installation, so one image runs everywhere
  and OTA images are no longer household-specific.

A device with no credentials does not fall back to defaults. It logs what is
missing every 10 seconds and joins nothing.

## Setup

On each programming computer, run from `uc/buttonsMeshIDF`:

```bash
python3 -m venv .pio/provisioning-venv
source .pio/provisioning-venv/bin/activate
python -m pip install -r provisioning/requirements.txt
```

Activate that environment again in each new terminal before provisioning.
On Debian/Ubuntu, if creating the environment fails because `venv` or
`ensurepip` is unavailable, install `python3-venv` first.

These tools work without a full ESP-IDF installation. Finding ESP-IDF's
`nvs_partition_gen.py` alone is not enough: newer versions are wrappers that
require the `esp_idf_nvs_partition_gen` Python package. The script checks both
tools before creating the credential image or accessing the device.

To select an interpreter without activating its environment, set
`PROVISION_PYTHON=/path/to/venv/bin/python` when running `provision.sh`.

## First time on a given device

With the environment above active, run from `provisioning/`:

```bash
cp credentials.csv.example credentials.csv
chmod 600 credentials.csv
$EDITOR credentials.csv
./provision.sh -p /dev/ttyUSB0
```

`provision.sh` reads the partition offset from `../partitions.csv`, builds an
NVS image and writes **only** that partition. The application is untouched, so
re-provisioning does not require reflashing the firmware.

The device must already have the current partition table (`partitions.csv`),
which contains `creds`. A device flashed from an older table logs
`No 'creds' partition` — do a normal `idf.py flash` once, then provision.

## Migrating a device that predates this

Existing builds have the values in `sdkconfig.<target>`. Lift them across
rather than retyping:

```bash
./from_sdkconfig.py ../sdkconfig.esp32-turbacz
./provision.sh -p /dev/ttyUSB0
```

`mesh_id` is truncated to 6 characters on purpose: only the first 6 ever
reached the air (`memcpy(mesh_id, CONFIG_MESH_ID, 6)`), so carrying the full
string over would form a different mesh than the devices already deployed.

Once every device is provisioned, delete the secret lines from
`sdkconfig.<target>` — nothing reads them any more, but they are still
credentials sitting on disk.

## Keys

| Key | Required | Notes |
|---|---|---|
| `router_ssid` | yes | WiFi the mesh root associates with |
| `router_pass` | yes | |
| `mesh_id` | yes | ≥6 characters; first 6 are used |
| `mesh_ap_pass` | yes | mesh-internal AP password |
| `mqtt_uri` | yes | e.g. `mqtt://192.168.1.100:1883` |
| `mqtt_user` | yes | |
| `mqtt_pass` | yes | |
| `ota_url` | no | turbacz's `/firmware/<device>.bin`; unset disables OTA |
| `ota_token` | no | must equal `[firmware].token` in `turbacz.toml` |

## Rotating credentials

Because nothing is compiled in, rotation no longer needs a rebuild: update
`credentials.csv`, re-run `provision.sh` on each device, and change the value
on the broker or router. The firmware image stays as it is.

## Note on confidentiality

NVS is not encrypted by default, so the values are readable by anyone who can
dump the flash. This removes the *remote* exposure — a binary passed around or
served over the network — not physical access. Enable NVS encryption and flash
encryption if that matters for your deployment.

## Battery remote switches and ESP-NOW gateways

One firmware image supports normal switches, normal relays, and C3 SuperMini
remote switches. Device role and gateway address are selected in the `creds`
partition when provisioning; OTA preserves them. Normal devices with no new
keys retain their existing mesh, buttons, LEDs and relay behavior.

1. Build/flash the current firmware on all possible mesh roots, and on the
   powered switches that will act as gateways. Update Turbacz as well.
2. Generate a shared authentication key with `python3 -c 'import secrets;
   print(secrets.token_hex(32))'`. Add the same `remote_key,data,string,<64 hex
   digits>` row to the credential CSV for **every possible root, gateway,
   and remote**, and provision those devices. Do not commit real keys.
3. Open RCM. An online normal switch has a **Gateway: off/on** button. Enable
   it and wait for device status to confirm. The gateway role persists in the
   ordinary NVS partition across restart and OTA. Erasing that partition
   disables gateway mode; credentials remain in their separate partition.
4. Hover over **Gateway: on** to obtain its **STA MAC address** and actual
   radio channel. This is not its numeric device ID: existing device IDs use
   the softAP MAC. Fix the Wi-Fi router's channel; channel changes require
   re-provisioning remotes.
5. For each C3 SuperMini remote, retain the installation/OTA credential rows
   and add:

   ```csv
   switch_role,data,string,remote
   gateway_mac,data,string,AA:BB:CC:DD:EE:FF
   remote_channel,data,u8,11
   remote_key,data,string,<same 64 hex digits>
   ```

   Flash the C3 firmware and run `provision.sh` with this device's CSV. Use
   `switch_role=normal` or omit it to restore a normal powered mesh switch.
   Keep `ota_url` pointing at the same C3 switch firmware, not a relay image.
6. After the first heartbeat, RCM registers the remote with seven buttons.
   Configure its momentary/stateful mappings and blind pairs as usual.

### Power, response time and retained functions

Remotes never start ESP-MESH, accept children or become root. They stop Wi-Fi
between interactions and enter **light sleep** until any of the seven existing
GPIOs changes level or a 60-second heartbeat expires. GPIO6/7 cannot wake a C3
from deep sleep, so light sleep preserves existing wiring and avoids a cold
boot/radio reinitialization on each press. Both press and release wake the
remote, including release of a long-held or stateful switch. ISR queues preserve
button edges during transmissions, and debounce uses the existing 50 ms
threshold. Long/short press payloads and the existing double-press/hold OTA
gesture remain supported.

The SuperMini's GPIO8 discrete, active-low blue LED is off in sleep and lights
briefly during sending or OTA. Remote mode does not run the normal WS2812 status
LED task. Remove/disconnect any always-on board power LED and measure regulator
idle current; software cannot eliminate those loads. Measure with USB unplugged.

Packets are authenticated with HMAC-SHA256/128; their contents are not encrypted.
Only the provisioned gateway MAC is accepted by the remote. Button retries use
the same boot session and sequence number. Updated roots suppress duplicates and
return an application ACK after handling the event. That ACK confirms root
processing, not relay actuation. Retry time is bounded (three 45 ms ACK windows),
and unavailable gateways/root nodes do not cause continuous scanning or retries.
A failed transaction is logged and counted; events are not retained indefinitely.

The **<200 ms button-to-relay latency is a hardware acceptance target, not a
verified guarantee**. The healthy path has 50 ms debounce, radio start,
ESP-NOW, mesh routing and relay execution; congestion, multiple simultaneous
buttons, retries or disconnected parents can exceed it. Before deploying:
measure press/release-to-relay latency after long idle, short taps, long holds,
stateful changes, lost ACKs, root changes, gateway restarts and gateway outage.
Check sleep current and average consumption with realistic usage as well.

Status is sent every minute and RCM allows 150 seconds before considering a
remote offline. OTA requests from RCM are retained by a reachable gateway until
its remote next wakes (up to about one heartbeat). The physical OTA gesture
starts directly. Gateway command storage holds the latest command per remote,
so avoid sending several maintenance commands at once. A gateway restart loses
pending maintenance requests; reissue them after a remote heartbeat. Immediate
unsolicited radio reception while asleep is intentionally unavailable.

Gateway mode adds ESP-NOW reception/forwarding without replacing local switch
buttons, LEDs, root eligibility or existing mesh services. No battery-side
scanning or automatic gateway selection is used: each remote has one explicitly
provisioned gateway.
