# Turbacz

Stable version of project **Domator**.

### Installation

1. Install uv with ```pip install uv```

2. Then copy this repository with ```git clone https://github.com/drdubel/domator```

3. Enter ```turbacz``` directory and run ```uv sync```

### Run MQTT Server

1. Install mosquitto:  
On Ubuntu:
```sudo apt install mosquitto mosquitto-clients```

2. Run ```sudo mosquitto_passwd -c /etc/mosquitto/passwd turbacz``` and insert new password for mqtt user ```turbacz```

3. Open ```/etc/mosquitto/mosquitto.conf``` and add

~~~
password_file /etc/mosquitto/passwd
allow_anonymous false
~~~

4. Run mqtt server with ```mosquitto -c /etc/mosquitto/mosquitto.conf```

### Run Webapp

1. Get Client ID and Client Secret following instructions from https://support.google.com/cloud/answer/6158849?hl=en

2. Create ```turbacz.toml``` file in the ```turbacz``` directory (you can copy ```turbacz.toml.example```) with:

```toml
authorized = ["YOUR_AUTHORIZED_EMAIL"]
jwt_secret = "YOUR_JWT_SECRET"
session_secret = "YOUR_SESSION_SECRET"

[mqtt]
password = "YOUR_MQTT_PASSWORD"

[oidc]
client_id = "YOUR_CLIENT_ID"
client_secret = "YOUR_CLIENT_SECRET"
allow_insecure_http = false
redirect_uri = "http://127.0.0.1:8000/auth"
token_endpoint_auth_method = "client_secret_post"

[psql]
dbname = "turbacz"
user = "turbacz"
password = "turbacz"
host = "127.0.0.1"
port = 5432
```

3. Run Webapp with ```uv run turbacz``` from ```turbacz``` directory

4. Open Turbacz on http://127.0.0.1:8000
#### It should work!

> If you need to run OIDC login over plain HTTP in local development, set:
> ```toml
> [oidc]
> allow_insecure_http = true
> ```
> Keep this `false` in production.
>
> If your app runs behind a reverse proxy/tunnel and you see `invalid_state` or
> `redirect_uri_mismatch`, set `[oidc].redirect_uri` to the exact callback URL
> registered in Google OAuth (same scheme/host/port/path, usually `/auth`).
>
> If you see `invalid_client`, verify `client_id` and `client_secret` are from
> the same OAuth app. Some providers also require
> `token_endpoint_auth_method = "client_secret_post"` (others use
> `client_secret_basic`).

### PostgreSQL setup (local)

On Ubuntu:

1. Install PostgreSQL:
```bash
sudo apt install postgresql postgresql-contrib
```
2. Create user and database:
```bash
sudo -u postgres psql -c "CREATE USER turbacz WITH PASSWORD 'turbacz';"
sudo -u postgres psql -c "CREATE DATABASE turbacz OWNER turbacz;"
```
3. Ensure your `turbacz.toml` `[psql]` section points to this database.

### Firmware / OTA

Uploaded OTA images are **not** public files. They embed the WiFi password and
the MQTT credentials that are compiled into the firmware, so serving them from
`static/` would publish those credentials to anyone who guessed the URL.

- Upload: `POST /upload/{switch,relay}`, logged-in session only. The image must
  start with the ESP application magic byte (`0xE9`); anything else is rejected.
- Download: `GET /firmware/{switch,relay}.bin`, which requires either a
  logged-in session or the `X-Firmware-Token` header matching
  `[firmware].token`.
- Images live in `[firmware].directory` (default `firmware/`, a Docker volume),
  never under `static/`. On startup the app moves any image left behind by an
  older version out of `static/data/` automatically.

Set `[firmware].token` in `turbacz.toml` and the same value as
`CONFIG_OTA_TOKEN` in `uc/buttonsMeshIDF` (`idf.py menuconfig` → Domator Mesh).
`setup.sh` generates the token and prints it. An empty token blocks device
downloads entirely rather than allowing all of them.

> Because these credentials are compiled into the image, anyone who obtains a
> firmware binary obtains the WiFi and MQTT passwords. Treat a leaked image as
> a full credential compromise: rotate the WiFi password, the mesh AP password
> and the broker passwords, then reflash.

### Required components checklist

- [ ] Python 3.14+
- [ ] uv
- [ ] `turbacz.toml` with filled `authorized`, `jwt_secret`, `session_secret`
- [ ] Valid Google OIDC `client_id` and `client_secret`
- [ ] `[firmware].token` set, matching `CONFIG_OTA_TOKEN` in the firmware
- [ ] PostgreSQL database reachable from `[psql]`
- [ ] MQTT broker reachable from `[mqtt]`

Quick checks:

```bash
python3 --version
uv --version
psql --version
mosquitto -h
```

### Docker setup (app + PostgreSQL + MQTT + Grafana + VictoriaMetrics)

For `uv sync` DNS failures during image builds on Armbian / Orange Pi, see the
[Docker troubleshooting guide](README-docker.md#troubleshooting-dependency-downloads-on-armbian--orange-pi).

The recommended way to run the entire system is through the unified Docker setup:

1. Run the project setup script:
   ```bash
   cd ..
   ./setup.sh
   ```

2. Follow the prompts to enter your authorized email

3. Start stack:
   ```bash
   docker compose up --build
   ```

Alternatively, from `turbacz` directory (deprecated approach):
1. Copy sample config:
```bash
cp turbacz.toml.example turbacz.toml
```
2. Update `authorized`, `jwt_secret`, `session_secret`, and OIDC values in `turbacz.toml`.
   Keep `[monitoring].metrics = "http://victoriametrics:8428"` for Docker setup.
3. Start stack:
```bash
docker compose up --build
```

This starts:
- `turbacz` web app on `http://127.0.0.1:8000`
- `postgres` database (`turbacz` / `turbacz`)
- `mosquitto` MQTT broker on port `1883`
- `grafana` on `http://127.0.0.1:3000` (default login: `admin` / `admin`)
- `victoriametrics` on `http://127.0.0.1:8428`
- `homeassistant` on `http://127.0.0.1:8123`

#### Host performance monitoring

Turbacz exports CPU utilization and time, load averages, memory and swap,
filesystem usage, disk and network throughput, uptime, optional hardware
temperatures, and its own process usage in Prometheus format at `/metrics`.
It uses the cross-platform `psutil` APIs and requires no root privileges. The
collector is enabled by default; disable it with:

```toml
[monitoring]
collect_host_metrics = false
```

The Docker stack configures VictoriaMetrics to scrape Turbacz every 15 seconds
and provisions Grafana with the **Domator / Host performance** dashboard. Open
`http://127.0.0.1:3000` after starting the stack. The first useful rate graphs
appear after two scrapes (about 30 seconds).

Grafana is pinned to a tested patch release instead of the moving `latest`
tag. Its embedded SQLite database uses write-ahead logging and lock retries so
Grafana 13's concurrent background services cannot turn a brief database lock
into an HTTP-server shutdown. The named `grafana_data` volume is retained
across container recreation.

There is an important scope distinction:

- A native `uv run turbacz` process reports the actual FreeBSD, Linux/Armbian,
  macOS, or Windows host as an unprivileged user. The default
  `host_metrics_scope = "auto"` resolves to `host` outside common containers;
  set it explicitly if the service sandbox hides container markers.
- A containerized process reports what its container namespace exposes. The
  supplied Docker configuration labels this as `scope="container"`; disk and
  process values in particular must not be interpreted as whole-host values.
  For accurate physical-host metrics without privileged containers, run
  Turbacz natively on that host and point the scraper at its port.

Temperature sensors are best-effort because many operating systems and boards
do not expose them to an unprivileged user. Their panel remains empty when the
API is unavailable; all other dashboard panels continue to work.

To scrape a native Turbacz instance from a separate VictoriaMetrics install,
copy `monitoring/prometheus.yml` and replace `turbacz:8000` with the machine's
reachable address. The same endpoint works with Prometheus or any other
Prometheus-compatible collector.

> The Docker MQTT broker (`mosquitto.conf`) requires authentication. `setup.sh` generates
> `mosquitto.passwd` from the credentials your firmware already uses; the broker will not
> start without it, so run `./setup.sh` before `docker compose up`.
> Per-user topic permissions live in `mosquitto.acl`.

### Home Assistant

Home Assistant ships with the Docker stack. Turbacz publishes lights, blinds, the heating loop
and wall-button events to it over MQTT Discovery, so entities appear automatically -- no custom
component, no YAML.

Enable it with `[ha] enabled = true` in `turbacz.toml` (or answer yes in `setup.sh`), run
`docker compose up -d --build turbacz homeassistant`, then add the MQTT integration in Home
Assistant with broker `mosquitto` port `1883`. See
[docs/home_assistant.md](../docs/home_assistant.md).
