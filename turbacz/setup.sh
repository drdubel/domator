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

echo "Generating configuration file..."

# Generate a random secret for JWT
JWT_SECRET=$(openssl rand -base64 32)
SESSION_SECRET=$(openssl rand -base64 32)

# Generate MQTT password
MQTT_PASSWORD=$(openssl rand -base64 32)

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
EOF

echo "Configuration file created successfully!"
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