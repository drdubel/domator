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
- Automatic 15-second scraping and a provisioned **Domator / Host performance** dashboard

The system will be accessible at http://localhost:8000

## Troubleshooting dependency downloads on Armbian / Orange Pi

If `uv sync` fails with `dns error` and `failed to lookup address information:
Try again`, the build could not resolve the package server's hostname. Check DNS
on the Orange Pi and in a container using the same base image:

```bash
getent hosts files.pythonhosted.org
docker run --rm python:3.14-slim-bookworm getent hosts files.pythonhosted.org
```

- If the first command fails, fix the host's network/DNS configuration first.
- If only the second fails, investigate Docker's DNS and bridge connectivity.
- If both succeed, retry `docker compose build turbacz`; the failure may have
  been temporary or specific to the build network.

The service's `dns:` setting applies to the running container. Dockerfile `RUN`
steps use the builder's network configuration; see Docker's
[service DNS](https://docs.docker.com/reference/compose-file/services/#dns) and
[build network](https://docs.docker.com/reference/compose-file/build/#network)
documentation.

When host DNS works, use this workaround with the local Docker Engine on Linux.
Run from the `turbacz` directory:

```bash
TURBACZ_BUILD_NETWORK=host docker compose build turbacz
docker compose up -d --no-build
```

This uses the host network for build steps. Services still use the Compose
network, including the `postgres` and `mosquitto` hostnames. The default build
network remains `default` when the variable is unset. A successful build with
this workaround does not verify connectivity from the running containers.

To check whether an explicit DNS server fixes container lookups:

```bash
docker run --rm --dns 1.1.1.1 python:3.14-slim-bookworm getent hosts files.pythonhosted.org
```

Use your LAN's DNS server instead if public DNS is unavailable. If this succeeds
while the ordinary container lookup fails, add the working resolver to Docker's
`/etc/docker/daemon.json`. For example, if `1.1.1.1` works:

```json
{
  "dns": ["1.1.1.1"]
}
```

Merge the `dns` key into any existing configuration, preserving the other keys.
Validate it, then restart Docker (this can interrupt running containers):

```bash
sudo dockerd --validate --config-file=/etc/docker/daemon.json
sudo systemctl restart docker
docker run --rm python:3.14-slim-bookworm getent hosts files.pythonhosted.org
docker compose build turbacz
docker compose up -d --no-build
```

Docker documents this in
[daemon DNS troubleshooting](https://docs.docker.com/engine/daemon/troubleshoot/#dns-resolver-issues).
If an explicit resolver also fails, investigate Docker bridge routing, IP
forwarding, and firewall rules before changing DNS again. A separately managed
BuildKit builder may also need its own DNS configuration.

## How It Works

The setup creates:
- A `turbacz.toml` configuration file with generated secure secrets
- Docker Compose configuration that links all services
- Proper volume mounts for persistent data storage

## Notes

- For full functionality, you'll need to configure Google OIDC credentials in the generated `turbacz.toml` file after first startup
- The MQTT broker requires authentication (`allow_anonymous false`); `setup.sh` generates `mosquitto.passwd`
- Uploaded OTA firmware is kept out of `static/` and served only through the authenticated `/firmware/<device>.bin` route
- All data is persisted through docker volumes
- Host metrics need no root access. In this Docker setup they describe the
  container-visible environment and carry `scope="container"`; run Turbacz
  directly on the machine for physical-host disk and OS namespace metrics.
