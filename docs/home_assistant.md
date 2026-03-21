# Home Assistant Integration — Phase 1

This document describes how to set up the Domator ↔ Home Assistant integration using MQTT Discovery.

## Architecture overview

```
Turbacz webapp  ──(HTTP API)──►  Domator backend  ──(MQTT)──►  MQTT broker  ◄──►  Home Assistant
(config UI)                       (HA bridge)                  (Mosquitto)          (app / dashboard)
```

- **Turbacz webapp** — configuration UI where you define homes, areas, devices, and capabilities.
- **Domator backend** — publishes HA MQTT Discovery configs and bridges state/commands.
- **Home Assistant** — auto-discovers entities and gives you dashboards, automations, and the mobile app.

---

## Prerequisites

| Component | Notes |
|-----------|-------|
| Home Assistant | Any recent release. [Install guide](https://www.home-assistant.io/installation/) |
| Mosquitto MQTT broker | Installable as an HA add-on or standalone |
| Domator / Turbacz | Running with PostgreSQL |

---

## 1. Install & configure Mosquitto

### Option A — Home Assistant add-on (easiest)

1. **Settings → Add-ons → Add-on Store → Mosquitto broker** → Install.
2. Enable **Start on boot** and **Watchdog**.
3. Create an MQTT user: **Settings → People → Users → Add user** (e.g. `mqtt_domator`).
4. Add the MQTT integration: **Settings → Devices & Services → Add Integration → MQTT**.
   - Host: `core-mosquitto` (when using the add-on)
   - User/password: the user you just created.

### Option B — standalone Mosquitto

```bash
# Debian/Ubuntu
sudo apt install mosquitto mosquitto-clients
```

Create `/etc/mosquitto/conf.d/domator.conf`:

```
listener 1883
allow_anonymous false
password_file /etc/mosquitto/passwd
```

Add credentials:

```bash
sudo mosquitto_passwd -c /etc/mosquitto/passwd mqtt_domator
sudo systemctl restart mosquitto
```

---

## 2. Configure Turbacz

Add the MQTT connection and enable the HA bridge in `turbacz.toml`:

```toml
[mqtt]
host = "192.168.1.50"   # your Mosquitto host
port = 1883
username = "mqtt_domator"
password = "your_password"

[ha]
enabled = true
```

Restart Turbacz.

---

## 3. Define your home in the Turbacz API

Use the REST API (or any HTTP client / the future Turbacz UI) to create entities.

### 3a. Create a home

```bash
curl -s -X POST http://localhost:8000/api/ha/homes \
  -H "Content-Type: application/json" \
  -b "access_token=<JWT>" \
  -d '{"name": "My Home"}'
```

Response:
```json
{"id": "a1b2c3d4-...", "name": "My Home", "areas": []}
```

Save the `id` — it is your **home_id**.

### 3b. Create an area (room)

```bash
curl -s -X POST http://localhost:8000/api/ha/areas \
  -H "Content-Type: application/json" \
  -b "access_token=<JWT>" \
  -d '{"home_id": "<home_id>", "name": "Living Room"}'
```

### 3c. Create a device

```bash
curl -s -X POST http://localhost:8000/api/ha/devices \
  -H "Content-Type: application/json" \
  -b "access_token=<JWT>" \
  -d '{"area_id": "<area_id>", "name": "Ceiling Light"}'
```

### 3d. Add capabilities

Supported `capability_type` values: `light`, `cover`, `climate`.

```bash
# Light
curl -s -X POST http://localhost:8000/api/ha/capabilities \
  -H "Content-Type: application/json" \
  -b "access_token=<JWT>" \
  -d '{"device_id": "<device_id>", "capability_type": "light", "name": "Living Room Light"}'

# Blinds (cover)
curl -s -X POST http://localhost:8000/api/ha/capabilities \
  -H "Content-Type: application/json" \
  -b "access_token=<JWT>" \
  -d '{"device_id": "<device_id>", "capability_type": "cover", "name": "Living Room Blinds"}'

# Heating (climate)
curl -s -X POST http://localhost:8000/api/ha/capabilities \
  -H "Content-Type: application/json" \
  -b "access_token=<JWT>" \
  -d '{"device_id": "<device_id>", "capability_type": "climate", "name": "Living Room Heating"}'
```

---

## 4. Apply to Home Assistant

Trigger the idempotent apply pipeline:

```bash
curl -s -X POST http://localhost:8000/api/ha/apply \
  -b "access_token=<JWT>"
```

Response:
```json
{
  "published": [
    "homeassistant/light/domator_<home_id>_<area_id>_<device_id>_light/config",
    "homeassistant/cover/domator_<home_id>_<area_id>_<device_id>_cover/config",
    "homeassistant/climate/domator_<home_id>_<area_id>_<device_id>_climate/config"
  ],
  "removed": [],
  "errors": []
}
```

Home Assistant will auto-discover the entities within seconds.

---

## 5. Verify in Home Assistant

1. **Settings → Devices & Services → MQTT** — you should see **My Home** device.
2. Entities listed:
   - `light.living_room_light`
   - `cover.living_room_blinds`
   - `climate.living_room_heating`
3. Add them to a dashboard: **Overview → Edit Dashboard → Add Card**.

---

## 6. Topic conventions

All topics follow this pattern:

| Purpose | Topic |
|---------|-------|
| Availability | `domator/{home_id}/status` |
| State | `domator/{home_id}/{area_id}/{device_id}/{type}/state` |
| Command | `domator/{home_id}/{area_id}/{device_id}/{type}/set` |
| Cover position state | `domator/{home_id}/{area_id}/{device_id}/cover/position` |
| Cover set position | `domator/{home_id}/{area_id}/{device_id}/cover/position/set` |
| Climate mode state | `domator/{home_id}/{area_id}/{device_id}/climate/mode/state` |
| Climate mode command | `domator/{home_id}/{area_id}/{device_id}/climate/mode/set` |
| Climate target temp state | `domator/{home_id}/{area_id}/{device_id}/climate/target/state` |
| Climate target temp cmd | `domator/{home_id}/{area_id}/{device_id}/climate/target/set` |
| Climate current temp | `domator/{home_id}/{area_id}/{device_id}/climate/current/state` |

`{home_id}`, `{area_id}`, `{device_id}` are stable UUIDs assigned at creation time and **never change**.

Discovery topics are published to `homeassistant/{type}/domator_{home_id}_{area_id}_{device_id}_{type}/config`.

---

## 7. Apply lifecycle (idempotent)

Call `POST /api/ha/apply` whenever you:

- Add a new device/capability.
- Remove a device/capability (Domator will publish an empty retained payload to the old discovery topic, which removes the entity from HA).
- Rename a capability (the `uniq_id` is stable, so HA updates the entity name).

The apply is **idempotent** — calling it multiple times produces the same result.

---

## 8. REST API reference

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/ha/homes` | List homes |
| `POST` | `/api/ha/homes` | Create home |
| `DELETE` | `/api/ha/homes/{id}` | Delete home (cascades) |
| `GET` | `/api/ha/homes/{home_id}/areas` | List areas in a home |
| `POST` | `/api/ha/areas` | Create area |
| `DELETE` | `/api/ha/areas/{id}` | Delete area (cascades) |
| `GET` | `/api/ha/areas/{area_id}/devices` | List devices in area |
| `POST` | `/api/ha/devices` | Create device |
| `DELETE` | `/api/ha/devices/{id}` | Delete device (cascades) |
| `GET` | `/api/ha/devices/{device_id}/capabilities` | List capabilities |
| `POST` | `/api/ha/capabilities` | Create capability |
| `DELETE` | `/api/ha/capabilities/{id}` | Delete capability |
| `GET` | `/api/ha/tree` | Full home/area/device/capability tree |
| `POST` | `/api/ha/apply` | Publish/clear HA discovery topics |

All endpoints require authentication (session cookie).

---

## 9. Running tests

```bash
cd turbacz
python -m pytest tests/test_ha.py -v
```

No MQTT broker or database required — all dependencies are mocked.

---

## 10. Security notes

- Use strong MQTT passwords; do not expose port 1883 to the internet.
- Enable TLS (`mqtts://`) on the broker for production deployments.
- Keep HA updated.
- Unique IDs are UUIDs — they never expose device names in MQTT topics.
