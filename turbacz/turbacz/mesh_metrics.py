"""Latest MQTT telemetry, exported on scrape without outbound writes.

Updates and collection run on the application event loop. Stale readings are
removed rather than repeatedly exported with fresh scrape timestamps.
"""

import math
import time
from dataclasses import dataclass

from aioprometheus.collectors import Gauge

from turbacz.settings import config

STALE_SECONDS = 30
FORGET_SECONDS = 3600

_FIELDS = {
    "uptime": ("node_info_uptime", "Device uptime in seconds"),
    "clicks": (
        "node_info_clicks",
        "Cumulative device clicks; resets on device restart",
    ),
    "freeHeap": ("node_info_free_heap", "Device free heap in bytes"),
    "ping": ("node_info_ping_time", "Device ping time in milliseconds"),
    "rssi": ("mesh_node_rssi", "Mesh Wi-Fi signal in dBm"),
}
readings = {
    field: Gauge(name, description) for field, (name, description) in _FIELDS.items()
}
available = Gauge(
    "node_info_available", "Whether device telemetry arrived within the last 30 seconds"
)
last_seen = Gauge(
    "node_info_last_seen_seconds", "Unix timestamp of the latest valid device telemetry"
)


@dataclass
class Snapshot:
    node_labels: dict
    mesh_labels: dict
    values: dict
    received: float
    timestamp: float


_snapshots: dict[str, Snapshot] = {}


def _label(value):
    value = str(value)
    if (
        not value
        or len(value) > 256
        or any(ord(c) < 32 or ord(c) == 127 for c in value)
    ):
        raise ValueError("Invalid metric label")
    # aioprometheus's text formatter inserts label values verbatim.
    return value.replace("\\", "\\\\").replace('"', '\\"')


def _number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float, str)):
        raise TypeError("Invalid metric value")
    value = float(value)
    if not math.isfinite(value):
        raise ValueError("Non-finite metric value")
    return value


def record_node_metrics(data, parent_name, ping):
    """Validate the complete report before replacing the previous snapshot."""
    common = {
        k: _label(v)
        for k, v in {
            **config.monitoring.labels,
            "id": data["deviceId"],
            "name": data["name"],
        }.items()
    }
    mesh = {
        **common,
        **{
            k: _label(v)
            for k, v in {
                "parent": data["parentId"],
                "parent_name": parent_name,
                "firmware": data["firmware"],
                "type": data["type"],
            }.items()
        },
    }
    values = {
        field: _number(data[field])
        for field in ("uptime", "clicks", "freeHeap", "rssi")
    }
    if ping is not None:
        values["ping"] = _number(ping)
    _snapshots[common["id"]] = Snapshot(
        common, mesh, values, time.monotonic(), time.time()
    )


def collect_mesh_metrics():
    """Rebuild label sets to remove expired or renamed/reparented series."""
    now = time.monotonic()
    for collector in (*readings.values(), available, last_seen):
        collector.values.clear()
    for device_id, snapshot in tuple(_snapshots.items()):
        age = now - snapshot.received
        if age > FORGET_SECONDS:
            del _snapshots[device_id]
            continue
        fresh = age <= STALE_SECONDS
        available.set(snapshot.node_labels, int(fresh))
        last_seen.set(snapshot.node_labels, snapshot.timestamp)
        if fresh:
            for field, value in snapshot.values.items():
                labels = (
                    snapshot.mesh_labels if field == "rssi" else snapshot.node_labels
                )
                readings[field].set(labels, value)
