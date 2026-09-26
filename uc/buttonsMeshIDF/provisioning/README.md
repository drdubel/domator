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
