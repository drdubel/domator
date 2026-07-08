#!/bin/bash

echo "Starting Domator Docker Setup..."
echo "================================"

echo "1. Checking Docker installation..."
if ! command -v docker &> /dev/null; then
    echo "Error: Docker is not installed. Please install Docker first."
    exit 1
fi

echo "2. Starting the system..."
cd /Users/antek/programowanie/domator/turbacz

# Start all services
docker-compose up -d

echo "3. Waiting for services to start..."
sleep 10

echo "4. Checking service status..."
echo "Services running:"
docker-compose ps

echo ""
echo "5. Setup complete!"
echo "The Domator system is now running with the following services:"
echo "- Turbacz backend on http://localhost:8000"
echo "- PostgreSQL database (turbacz/turbacz)"
echo "- Mosquitto MQTT broker on port 1883"
echo "- Grafana dashboard on http://localhost:3000 (default login: admin/admin)"
echo "- VictoriaMetrics monitoring on http://localhost:8000"
echo ""
echo "To stop the system, run: docker-compose down"