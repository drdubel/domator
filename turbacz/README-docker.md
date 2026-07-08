# Docker Setup for Turbacz

This project now supports a streamlined Docker-based deployment that allows running the entire system with a single command.

## Quick Start

1. Navigate to the turbacz directory:
   ```bash
   cd turbacz
   ```

2. Run the setup script:
   ```bash
   ./setup.sh
   ```

3. Follow the prompts to enter your authorized email address

4. Start the system:
   ```bash
   docker-compose up
   ```

## What This Provides

- FastAPI backend web application
- PostgreSQL database (turbacz/turbacz)
- Mosquitto MQTT broker
- Grafana dashboard (http://localhost:3000) - default login: admin/admin
- VictoriaMetrics monitoring

The system will be accessible at http://localhost:8000

## How It Works

The setup creates:
- A `turbacz.toml` configuration file with generated secure secrets
- Docker Compose configuration that links all services
- Proper volume mounts for persistent data storage

## Notes

- For full functionality, you'll need to configure Google OIDC credentials in the generated `turbacz.toml` file after first startup
- The MQTT broker allows anonymous access for development (not suitable for production)
- All data is persisted through docker volumes