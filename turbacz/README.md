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

### Required components checklist

- [ ] Python 3.14+
- [ ] uv
- [ ] `turbacz.toml` with filled `authorized`, `jwt_secret`, `session_secret`
- [ ] Valid Google OIDC `client_id` and `client_secret`
- [ ] PostgreSQL database reachable from `[psql]`
- [ ] MQTT broker reachable from `[mqtt]`

Quick checks:

```bash
python3 --version
uv --version
psql --version
mosquitto -h
```

### Docker setup (app + PostgreSQL + MQTT)

From `turbacz` directory:

1. Copy sample config:
```bash
cp turbacz.toml.example turbacz.toml
```
2. Update `authorized`, `jwt_secret`, `session_secret`, and OIDC values in `turbacz.toml`.
3. Start stack:
```bash
docker compose up --build
```
4. (Optional, if binding to host from Docker) set:
```toml
[server]
host = "0.0.0.0"
port = 8000
```

This starts:
- `turbacz` web app on `http://127.0.0.1:8000`
- `postgres` database (`turbacz` / `turbacz`)
- `mosquitto` MQTT broker on port `1883`
