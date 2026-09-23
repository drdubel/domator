#!/bin/bash

echo "Domator Docker Setup"
echo "==================="

# Check if docker is installed
if ! command -v docker &> /dev/null; then
    echo "Error: Docker is not installed. Please install Docker first."
    exit 1
fi

# Create the config file with user prompts
echo "Please enter your email address that will be authorized to access the system:"
read -p "> " AUTHORIZED_EMAIL

echo ""
echo "Expose devices to Home Assistant via MQTT Discovery? [Y/n]"
read -p "> " ENABLE_HA
case "$ENABLE_HA" in
    [Nn]*) HA_ENABLED="false" ;;
    *)     HA_ENABLED="true" ;;
esac

echo "Generating configuration file..."

# Generate a random secret for JWT
JWT_SECRET=$(openssl rand -base64 32)
SESSION_SECRET=$(openssl rand -base64 32)

# Generate MQTT password
MQTT_PASSWORD=$(openssl rand -base64 32)

# Token the microcontrollers present (X-Firmware-Token) to download OTA
# images. Those images contain WiFi and MQTT credentials, so the download
# endpoint is never anonymous. Must match CONFIG_OTA_TOKEN in the firmware.
FIRMWARE_TOKEN=$(openssl rand -hex 32)

# Home Assistant connects to the same broker as its own user, restricted by
# mosquitto.acl to the homeassistant/ and domator/ trees.
HA_MQTT_PASSWORD=$(openssl rand -base64 24 | tr -d '/+=')

# Create the turbacz.toml config file
cat > turbacz.toml << EOF
authorized = ["$AUTHORIZED_EMAIL"]
jwt_secret = "$JWT_SECRET"
session_secret = "$SESSION_SECRET"
use_mqtt = true

[mqtt]
host = "mosquitto"
port = 1883
username = "turbacz"
password = "$MQTT_PASSWORD"

[oidc]
client_id = ""
client_secret = ""
allow_insecure_http = true
redirect_uri = "http://127.0.0.1:8000/auth"
token_endpoint_auth_method = "client_secret_post"

[psql]
dbname = "turbacz"
user = "turbacz"
password = "turbacz"
host = "postgres"
port = 5432

[server]
host = "0.0.0.0"
port = 8000

[monitoring]
metrics = "http://victoriametrics:8428"
collect_host_metrics = true
host_metrics_scope = "container"

[firmware]
directory = "firmware"
token = "$FIRMWARE_TOKEN"

[ha]
enabled = $HA_ENABLED
EOF

echo "Configuration file created successfully!"
echo ""
echo "=============================================================="
echo " Firmware OTA token -- set this as CONFIG_OTA_TOKEN in"
echo " uc/buttonsMeshIDF (idf.py menuconfig -> Domator Mesh) before"
echo " flashing, or the devices cannot download updates:"
echo ""
echo "   $FIRMWARE_TOKEN"
echo "=============================================================="
echo ""
# ---------------------------------------------------------------------------
# Broker credentials
#
# The broker is shared between the ESP mesh, the backend and Home Assistant, so
# anonymous access is off. Firmware credentials are compiled in, so the
# password file has to match what the devices already use -- getting these
# wrong means reflashing.
# ---------------------------------------------------------------------------

if [ -f mosquitto.passwd ]; then
    echo ""
    echo "mosquitto.passwd already exists -- leaving it alone."
    echo "Delete it and re-run this script to regenerate broker credentials."
else
    echo ""
    echo "Setting up broker credentials."
    echo "These must match what is compiled into your firmware."
    echo ""
    # No default here on purpose: a password baked into this script is a
    # password published to everyone who clones the repository.
    echo "Mesh root password (uc/buttonsMeshIDF, CONFIG_MQTT_PASSWORD)"
    echo "  must match sdkconfig.<target> -- copy it from there:"
    while [ -z "${MESH_PASSWORD:-}" ]; do
        read -r -p "> " MESH_PASSWORD
        [ -z "$MESH_PASSWORD" ] && echo "  Required. Read it out of your sdkconfig and paste it here."
    done

    echo "Heating controller password (uc/heating, credentials.h) -- blank to skip:"
    read -p "> " HEATING_PASSWORD

    echo "Legacy blinds controller password (uc/blinds_wifi, credentials.h) -- blank to skip:"
    read -p "> " BLINDS_PASSWORD

    : > mosquitto.passwd
    chmod 600 mosquitto.passwd
    {
        echo "turbacz:$MQTT_PASSWORD"
        echo "mesh_root:$MESH_PASSWORD"
        [ -n "$HEATING_PASSWORD" ] && echo "heating-wifi:$HEATING_PASSWORD"
        [ -n "$BLINDS_PASSWORD" ] && echo "blinds-wifi:$BLINDS_PASSWORD"
        [ "$HA_ENABLED" = "true" ] && echo "homeassistant:$HA_MQTT_PASSWORD"
    } >> mosquitto.passwd

    # mosquitto_passwd -U hashes the file in place. Mount the directory rather
    # than the file: -U writes a temp file and renames over the original, which
    # cannot be done to a bind-mounted file. Run as the invoking user so the
    # result stays writable on the host.
    docker run --rm -v "$PWD:/work" -w /work --user "$(id -u):$(id -g)" \
        eclipse-mosquitto:2.0 mosquitto_passwd -U mosquitto.passwd
    if [ $? -ne 0 ]; then
        echo "ERROR: could not hash mosquitto.passwd -- it still holds plaintext."
        echo "Hash it manually with: mosquitto_passwd -U mosquitto.passwd"
        exit 1
    fi
    echo "Broker credentials written to mosquitto.passwd"

    if [ "$HA_ENABLED" = "true" ]; then
        echo ""
        echo "=============================================================="
        echo " Home Assistant MQTT credentials -- save these now:"
        echo "   username: homeassistant"
        echo "   password: $HA_MQTT_PASSWORD"
        echo ""
        echo " Add the MQTT integration in Home Assistant pointing at this"
        echo " broker on port 1883. Entities appear automatically."
        echo " See docs/home_assistant.md"
        echo "=============================================================="
    fi
fi

echo ""
echo "To start the Domator system, run:"
echo "  docker-compose up"
echo ""
echo "Note: For full functionality, you'll need to set up Google OIDC credentials in turbacz.toml"
echo "      after starting the container for the first time."
echo ""
echo "If you encounter any issues with Docker images, ensure Docker is running and try:"
echo "  docker-compose pull"
echo "  docker-compose up"
