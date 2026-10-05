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
- Your **Domator / Światłomesz2** mesh dashboard, provisioned from
  `monitoring/grafana/dashboards/swiatlomesz.json` and connected to VictoriaMetrics.
  It defaults to all apartments; narrow the panels with **Mieszkanie** and
  **Device ID**. Device names remain visible in charts and on the map.

### Mesh dashboard

After copying the updated `monitoring` directory to the device, run from `turbacz`:

```bash
docker compose up -d victoriametrics grafana
```

Open `http://DEVICE_IP:3000/d/mix8n8g` or find **Światłomesz2** in the
**Domator** folder. An already-running Grafana picks up dashboard file changes
within about 30 seconds. This file uses Classic dashboard JSON for the existing
file provisioning setup; the original v2 export's data-source references were
replaced with the provisioned `victoriametrics` UID. The map creates unique
edge IDs and uses apartment-qualified device IDs for its source and target.

The mesh dashboard starts with reporting-device count, weak-link count,
weakest signal, highest ping, lowest free heap, and estimated clicks during the
selected time range. Compact sections cover named topology, RSSI and ping
history, free heap, button activity, uptime, observed restarts, and firmware
inventory. Click rates account for counter resets. Uptime decreases indicate
observed restarts; restarts during telemetry gaps may be missed.

Most current mesh readings use a two-minute window; the topology uses 30
seconds. These are telemetry windows, not authoritative online/offline states.
The previous disconnect and dropped-message charts were replaced because the
current broker does not export those metrics. Missing readings remain missing
rather than displaying a healthy zero. RSSI values of zero are excluded because
the firmware also uses zero when signal information is unavailable.

**Host performance** separates scrape reachability from collector health and
adds available memory, scrape/collection duration, network errors and drops,
Wi-Fi history, free disk space, and process threads/open files. The process
memory chart has a separate axis from CPU. Check **Measured scope**: bridge-mode
Docker resource metrics describe the container-visible environment, while the
optional Wi-Fi mount supplies physical-host wireless statistics.

Both dashboards have navigation links and preserve gaps in time-series data.
Dashboards refresh once per minute by default to limit query bursts on small
boards. You can select a faster refresh in Grafana when troubleshooting.
Threshold colors are visual troubleshooting guides, not configured alerts.
The Prometheus data source uses the same 15-second interval as the scraper.

### All telemetry uses `/metrics`

VictoriaMetrics scrapes mesh, heating and host metrics from the same endpoint.
The bundled Compose setup already configures a 15-second scrape. For an external
VictoriaMetrics instance, add this job to the file loaded by its
`-promscrape.config` flag and reload/restart that instance:

```yaml
scrape_configs:
  - job_name: turbacz
    scrape_interval: 15s
    static_configs:
      - targets: ["TURBACZ_HOST_IP:8000"]
```

The target must be reachable from VictoriaMetrics. Native Turbacz must listen
on a reachable interface, such as `server.host = "0.0.0.0"`. Grafana must query
this same VictoriaMetrics instance. Setting `monitoring.metrics` alone does not
configure scraping: that URL is now used only for heating-history queries.
The obsolete `monitoring.send_metrics` setting is ignored; no push path remains.

Existing mesh metric names and device labels are preserved. Scraped series also
receive the scraper's `job` and `instance` labels, so old pushed history and new
scraped data have different series identities. Avoid running old push-producing
Turbacz instances alongside the new one for the same devices. Existing history
is retained, but a dashboard range spanning migration can show both identities.

Mesh readings expire after 30 seconds without valid telemetry. The endpoint
continues to expose `node_info_available = 0` and
`node_info_last_seen_seconds` for up to one hour, then forgets the device.
Renames, firmware changes and parent changes replace old label sets on the next
scrape. Scrapes use collection timestamps and keep only the latest report;
short-lived changes between scrapes are not retained. Dashboard range windows
can still display earlier samples until their lookback window expires.

Verify after deploying and receiving a device report:

```bash
curl -fsS http://localhost:8000/metrics | grep -E '^(node_info_|mesh_node_)'
```

### Idle CPU usage

Turbacz stores the latest mesh readings in memory and exposes them alongside
heating and host telemetry at `/metrics`, with no outbound metric writes. One
HTTP connection pool is retained for heating-history queries. Device names are
cached for up to 60 seconds;
edits through Turbacz invalidate this cache immediately. MQTT reconnects reuse
the existing device-check and Home Assistant background tasks. Duplicate relay
state reports no longer trigger WebSocket broadcasts to every open page.

To apply backend changes, rebuild the service from the updated checkout:

```bash
docker compose up -d --build --no-deps turbacz
```

This briefly restarts Turbacz. Dashboard files are picked up automatically
within about 30 seconds; reload open dashboards to use the new refresh default.
Compare `docker stats` with pages closed and then open. CPU percentages there
are relative to one core. Collect several minutes of samples to account for
15-second telemetry bursts; a single snapshot is not an idle average.

After deploying dashboard updates, ensure Grafana can read the files:

```bash
chmod 755 monitoring/grafana/dashboards monitoring/grafana/provisioning \
  monitoring/grafana/provisioning/dashboards monitoring/grafana/provisioning/datasources
chmod 644 monitoring/grafana/dashboards/*.json
chmod 644 monitoring/grafana/provisioning/dashboards/dashboards.yml \
  monitoring/grafana/provisioning/datasources/victoriametrics.yml
docker compose restart grafana
```

Restarting also loads changes to data-source provisioning. Dashboard JSON-only
changes normally appear within 30 seconds without a restart. Use a browser
refresh to reload an already-open dashboard.

If the logs report `Datasource provisioning error` with `permission denied`,
Grafana cannot read the mounted YAML and may repeatedly restart. Apply the
permissions above on the Docker host (use `sudo` if another user owns the
files). Fixing only the dashboard JSON permissions is insufficient. These
commands target the shipped dashboard and provisioning files, not the database
volume or application credential files.

The panels require the device metrics `mesh_node_rssi` and `node_info_*` in
VictoriaMetrics. Importing the dashboard does not transfer historical metrics.
Keep persistent dashboard edits in the JSON file: later provisioning updates
can overwrite edits made only in the Grafana UI.

The system will be accessible at http://localhost:8000

## Host Wi-Fi signal on Armbian / Linux

The **Host performance** dashboard includes a **Host Wi-Fi signal strength**
indicator in dBm for each wireless interface, filtered by the selected Instance.
For Docker on the Orange Pi, first check that the driver exposes a negative
signal level in the host's wireless statistics:

```bash
cat /proc/net/wireless
```

Enable the optional read-only host statistics mount and rebuild Turbacz:

```bash
docker compose -f docker-compose.yml -f docker-compose.wifi.yml up -d --build turbacz grafana
```

Use both Compose files for subsequent stack updates to retain the mount.
The override reads `/proc/1/net/wireless` from the Linux host network namespace;
it does not require privileged containers or host networking. Native Linux
installations read `/proc/net/wireless` automatically. The bar colors change
at -80, -70, and -60 dBm; less negative values mean stronger reception.
Ethernet-only hosts, unsupported drivers, and disconnected interfaces without
valid readings show no data. This optional collector depends on the driver's
wireless-extension statistics and does not collect Wi-Fi on macOS or Windows.

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
