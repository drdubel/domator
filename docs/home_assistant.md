# Home Assistant integration

Turbacz can publish its devices to Home Assistant over [MQTT
Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery).
Entities appear automatically — there is no custom component to install and no
YAML to write.

Home Assistant is treated as a **user** surface: lights, blinds, the heating
loop and wall buttons. Administration — firmware updates, mesh topology, wiring
and naming — stays in the Turbacz web UI.

## How it fits together

```
ESP mesh ──/relay/state/…──► Mosquitto ──► Turbacz ──► domator/light/…/state ──► Home Assistant
                                              ▲                                        │
                                              └────────── domator/light/…/set ◄─────────┘
```

Home Assistant talks to the same Mosquitto instance as the mesh, but the ACL
confines it to the `homeassistant/` and `domator/` topic trees. It never sees
or commands the mesh protocol directly — everything goes through Turbacz.

## Setup

Home Assistant runs as part of the Docker stack, so there is nothing to install
separately.

### 1. Enable the bridge

Add to `turbacz.toml`:

```toml
[ha]
enabled = true
```

`setup.sh` asks about this on a fresh install. On an existing one, edit the file
by hand -- **do not re-run `setup.sh`, it overwrites `turbacz.toml`.**

### 2. Start Home Assistant

```bash
cd turbacz
docker compose up -d --build turbacz homeassistant
```

The rebuild is needed whenever Turbacz's Python code changes: `turbacz.toml` is
bind-mounted and picked up live, but the code is baked into the image. A stale
image plus a new `[ha]` block fails at startup with
`Extra inputs are not permitted`.

Home Assistant's config lives in `turbacz/homeassistant/`, bind-mounted into the
container. `configuration.yaml` is seeded; everything else it writes there
(database, logs, `.storage`) is gitignored, because `.storage` holds auth
tokens.

### 3. Add the MQTT integration

Open `http://<host>:8123`, create your Home Assistant account, then
**Settings → Devices & Services → Add Integration → MQTT**:

| Field | Value |
|---|---|
| Broker | `mosquitto` |
| Port | `1883` |
| Username / password | only if you have enabled them in `mosquitto.passwd` |

The broker is **`mosquitto`**, the compose service name -- not an IP. Home
Assistant shares the compose network, so it resolves the broker by name.

This is the one manual step. A containerised Home Assistant has no Supervisor,
so its MQTT broker connection can only be set up through the UI config flow.

Your devices appear immediately: the discovery topics are retained, so they are
waiting on the broker regardless of what order things started in.

### 4. Optional -- broker authentication

The shipped `mosquitto.conf` requires authentication and `mosquitto.acl`
confines the `homeassistant` user to the `homeassistant/` and `domator/` trees.
See "Locking down the broker" below. Get everything working first, then add
credentials as a separate step.

## What you get

Devices are grouped by **section**, the same rooms you define in the Turbacz
lights UI. Renaming a section in Turbacz renames the device in Home Assistant.

| Turbacz | Home Assistant | Notes |
|---|---|---|
| Named relay output | `light` | On/off only; the hardware has no dimming |
| Blind pair | `cover` | Open / close / stop |
| Heating loop | `water_heater` + `sensor` | Mixed temp and setpoint; probes and PID as sensors |
| Wall button | `event` | For automations; the mesh still switches lights on its own |

Buttons group under their physical wall panel rather than a room, since a panel
is not in one.

### Naming

**Only outputs you have named appear in Home Assistant.** An output still
called `Output 3` is treated as unconfigured and is skipped. Name it in the
Turbacz lights UI and it shows up on the next sync (within a minute, usually
immediately).

Renaming an output or moving it to another section keeps its Home Assistant
history: entities are keyed on relay and output id, never on the name or room.

### Availability

Entities go *Unavailable* when the relay board stops reporting (about 45
seconds) or when Turbacz itself is down. This is a real unavailable state, not
a stale last-known value.

## Locking down the broker

The broker is shared with the ESP mesh, so anonymous access is off by default.
`setup.sh` builds `mosquitto.passwd` from the credentials your firmware already
uses. To add Home Assistant to an existing password file:

```bash
cd turbacz
docker run --rm -v "$PWD:/work" -w /work --user "$(id -u):$(id -g)" \
  eclipse-mosquitto:2.0 mosquitto_passwd -b mosquitto.passwd homeassistant <password>
docker compose restart mosquitto
```

Then set that username and password in the MQTT integration.

The password file must also contain the accounts your firmware uses --
`turbacz`, `mesh_root`, `heating-wifi` and `blinds-wifi`. Miss one and that
device is locked out and goes silent. No reflashing is needed as long as the
passwords match what is already compiled in.

## Limitations

- **Blinds report no position.** The hardware has no encoder, so Home Assistant
  gets open / closed / opening / closing, inferred from which way the blind was
  last driven. After a Turbacz restart a blind reads as *unknown* until it next
  moves. There is no "go to 40%".
- **The legacy `r1`–`r7` blinds are not exposed.** They are the only blinds with
  real 0–999 position feedback, but they are hardcoded in the web UI with no
  registry entry to derive entities from.
- **No heating modes.** The controller is a PID mixing valve with no on/off or
  mode concept, so the `water_heater` entity has a single fixed operation mode.
- **PID tuning is read-only.** `kp`, `ki`, `kd` and the integral are exposed as
  diagnostic sensors. Tune from the Turbacz UI — a stray automation should not
  be able to destabilise the heating loop.
- **No firmware updates or mesh diagnostics in Home Assistant**, by design.

## Known security debt

The mesh root's broker password is compiled into
`uc/buttonsMeshIDF/sdkconfig.esp32-turbacz` and is therefore in git history.
Rotating it means reflashing every mesh node, so it has been left as-is. The
ACL limits the damage: that account can only reach the mesh topics.

## Troubleshooting

**No entities appear.** Check `enabled = true` and that Turbacz reached the
broker:

```bash
mosquitto_sub -h <broker> -u homeassistant -P <password> -t 'homeassistant/#' -v
```

You should see one retained config per entity plus `domator/status online`.

**An entity is stuck Unavailable.** Either the relay board is offline, or
Turbacz is. Check `domator/status` and `domator/availability/relay_<id>`.

**A deleted entity lingers.** Turbacz clears discovery topics when an entity
disappears. If one survives, clear the retained message by hand:

```bash
mosquitto_pub -h <broker> -u homeassistant -P <password> -r -n \
  -t 'homeassistant/light/domator_light_<relay>_<output>/config'
```

**Test without Home Assistant.** Commands are plain MQTT:

```bash
mosquitto_pub -h <broker> -u homeassistant -P <password> \
  -t 'domator/light/1234567890123_a/set' -m 'ON'
```
