import logging
import os
import platform
import socket
import time

import psutil
from aioprometheus.collectors import Counter, Gauge

from turbacz.settings import config

logger = logging.getLogger(__name__)


def _gauge(name: str, description: str) -> Gauge:
    return Gauge(name, description, const_labels=config.monitoring.labels)


def _counter(name: str, description: str) -> Counter:
    return Counter(name, description, const_labels=config.monitoring.labels)

water_temp = Gauge(
    "water_temperature",
    "Water temperature in Celsius",
    const_labels=config.monitoring.labels,
)
pid_integral = Gauge("pid_integral", "PID integral accumulator", const_labels=config.monitoring.labels)
pid_output = Gauge("pid_output", "PID output", const_labels=config.monitoring.labels)
pid_target = Gauge("pid_target", "PID target", const_labels=config.monitoring.labels)
pid_multiplier = Gauge("pid_multiplier", "PID multiplier", const_labels=config.monitoring.labels)

# These metrics deliberately use psutil rather than Linux-only /proc parsing or
# privileged hardware interfaces. The same collector therefore works when the
# service is run by an ordinary user on Linux (including Armbian), FreeBSD,
# macOS, and Windows. Optional platform APIs are simply omitted when absent.
host_info = _gauge("turbacz_host_info", "Static information about the measured host")
host_metrics_collection_success = _gauge(
    "turbacz_host_metrics_collection_success",
    "Whether the latest host metrics collection completed successfully",
)
host_metrics_collection_duration_seconds = _gauge(
    "turbacz_host_metrics_collection_duration_seconds",
    "Time spent collecting host metrics",
)
host_boot_time_seconds = _gauge("turbacz_host_boot_time_seconds", "Host boot time as a Unix timestamp")
host_uptime_seconds = _gauge("turbacz_host_uptime_seconds", "Seconds since the host booted")
host_cpu_usage_percent = _gauge(
    "turbacz_host_cpu_usage_percent",
    "Host CPU utilization percentage since the previous scrape",
)
host_cpu_count = _gauge("turbacz_host_cpu_count", "Number of host CPUs by kind")
host_cpu_seconds = _counter("turbacz_host_cpu_seconds", "Cumulative host CPU time by mode")
host_load_average = _gauge("turbacz_host_load_average", "Host load average")
host_load_average_per_cpu = _gauge(
    "turbacz_host_load_average_per_cpu",
    "Host load average divided by logical CPU count",
)
host_memory_bytes = _gauge("turbacz_host_memory_bytes", "Host memory bytes by state")
host_memory_usage_percent = _gauge("turbacz_host_memory_usage_percent", "Host memory utilization percentage")
host_swap_bytes = _gauge("turbacz_host_swap_bytes", "Host swap bytes by state")
host_swap_usage_percent = _gauge("turbacz_host_swap_usage_percent", "Host swap utilization percentage")
host_disk_bytes = _gauge("turbacz_host_disk_bytes", "Filesystem bytes by state")
host_disk_usage_percent = _gauge("turbacz_host_disk_usage_percent", "Filesystem utilization percentage")
host_disk_read_bytes = _counter("turbacz_host_disk_read_bytes_total", "Bytes read from a block device")
host_disk_written_bytes = _counter("turbacz_host_disk_written_bytes_total", "Bytes written to a block device")
host_network_received_bytes = _counter(
    "turbacz_host_network_received_bytes_total",
    "Bytes received by a network interface",
)
host_network_sent_bytes = _counter("turbacz_host_network_sent_bytes_total", "Bytes sent by a network interface")
host_network_received_packets = _counter(
    "turbacz_host_network_received_packets_total",
    "Packets received by a network interface",
)
host_network_sent_packets = _counter(
    "turbacz_host_network_sent_packets_total",
    "Packets sent by a network interface",
)
host_network_errors = _counter("turbacz_host_network_errors_total", "Network errors by interface and direction")
host_network_drops = _counter("turbacz_host_network_drops_total", "Dropped packets by interface and direction")
host_temperature_celsius = _gauge("turbacz_host_temperature_celsius", "Hardware temperature when exposed by the OS")
process_cpu_usage_percent = _gauge(
    "turbacz_process_cpu_usage_percent",
    "Turbacz process CPU usage as a percentage of one logical CPU",
)
process_resident_memory_bytes = _gauge(
    "turbacz_process_resident_memory_bytes",
    "Turbacz process resident memory",
)
process_virtual_memory_bytes = _gauge(
    "turbacz_process_virtual_memory_bytes",
    "Turbacz process virtual memory",
)
process_threads = _gauge("turbacz_process_threads", "Number of Turbacz process threads")
process_open_files = _gauge("turbacz_process_open_files", "Number of files opened by the Turbacz process")

_process = psutil.Process(os.getpid())
# Prime the non-blocking samplers. Future calls report utilization since this
# point (or the preceding scrape) without delaying the HTTP request.
try:
    psutil.cpu_percent(interval=None)
    _process.cpu_percent(interval=None)
except (OSError, psutil.Error):
    # A restrictive service sandbox may not allow sampling until later. The
    # first permitted scrape will simply report the psutil baseline value.
    pass


def _set_memory_metrics() -> None:
    memory = psutil.virtual_memory()
    for state in ("total", "available", "used", "free", "active", "inactive", "buffers", "cached"):
        value = getattr(memory, state, None)
        if value is not None:
            host_memory_bytes.set({"state": state}, value)
    host_memory_usage_percent.set({}, memory.percent)

    try:
        swap = psutil.swap_memory()
    except (OSError, PermissionError, psutil.AccessDenied, NotImplementedError):
        return
    for state in ("total", "used", "free"):
        host_swap_bytes.set({"state": state}, getattr(swap, state))
    host_swap_usage_percent.set({}, swap.percent)


def _set_disk_metrics() -> None:
    for partition in psutil.disk_partitions(all=False):
        try:
            usage = psutil.disk_usage(partition.mountpoint)
        except (OSError, PermissionError, psutil.AccessDenied):
            continue
        labels = {
            "device": partition.device,
            "mountpoint": partition.mountpoint,
            "fstype": partition.fstype or "unknown",
        }
        for state in ("total", "used", "free"):
            host_disk_bytes.set({**labels, "state": state}, getattr(usage, state))
        host_disk_usage_percent.set(labels, usage.percent)

    try:
        counters = psutil.disk_io_counters(perdisk=True) or {}
    except (OSError, PermissionError, psutil.AccessDenied, NotImplementedError):
        counters = {}
    for device, values in counters.items():
        host_disk_read_bytes.set({"device": device}, values.read_bytes)
        host_disk_written_bytes.set({"device": device}, values.write_bytes)


def _set_network_metrics() -> None:
    for interface, values in psutil.net_io_counters(pernic=True).items():
        labels = {"interface": interface}
        host_network_received_bytes.set(labels, values.bytes_recv)
        host_network_sent_bytes.set(labels, values.bytes_sent)
        host_network_received_packets.set(labels, values.packets_recv)
        host_network_sent_packets.set(labels, values.packets_sent)
        host_network_errors.set({**labels, "direction": "receive"}, values.errin)
        host_network_errors.set({**labels, "direction": "send"}, values.errout)
        host_network_drops.set({**labels, "direction": "receive"}, values.dropin)
        host_network_drops.set({**labels, "direction": "send"}, values.dropout)


def _set_temperature_metrics() -> None:
    sensor_reader = getattr(psutil, "sensors_temperatures", None)
    if sensor_reader is None:
        return
    try:
        sensors = sensor_reader(fahrenheit=False)
    except (OSError, RuntimeError, psutil.AccessDenied, NotImplementedError):
        return
    for chip, entries in sensors.items():
        for index, entry in enumerate(entries):
            host_temperature_celsius.set(
                {"chip": chip, "sensor": entry.label or str(index)},
                entry.current,
            )


def _set_process_metrics() -> None:
    memory = _process.memory_info()
    process_cpu_usage_percent.set({}, _process.cpu_percent(interval=None))
    process_resident_memory_bytes.set({}, memory.rss)
    process_virtual_memory_bytes.set({}, memory.vms)
    process_threads.set({}, _process.num_threads())
    try:
        process_open_files.set({}, len(_process.open_files()))
    except (psutil.AccessDenied, NotImplementedError):
        pass


def _set_cpu_metrics() -> None:
    host_cpu_usage_percent.set({}, psutil.cpu_percent(interval=None))

    logical_cpus = psutil.cpu_count(logical=True) or 1
    host_cpu_count.set({"kind": "logical"}, logical_cpus)
    physical_cpus = psutil.cpu_count(logical=False)
    if physical_cpus is not None:
        host_cpu_count.set({"kind": "physical"}, physical_cpus)

    for mode, seconds in psutil.cpu_times()._asdict().items():
        host_cpu_seconds.set({"mode": mode}, seconds)

    try:
        loads = psutil.getloadavg()
    except (AttributeError, OSError):
        loads = ()
    for period, load in zip(("1m", "5m", "15m"), loads):
        host_load_average.set({"period": period}, load)
        host_load_average_per_cpu.set({"period": period}, load / logical_cpus)


def _set_uptime_metrics() -> None:
    # Some restricted sandboxes block the OS query used by boot_time even for
    # an ordinary user. Uptime is optional in that case; other categories must
    # still be exported.
    try:
        boot_time = psutil.boot_time()
    except (OSError, PermissionError, psutil.AccessDenied):
        return
    host_boot_time_seconds.set({}, boot_time)
    host_uptime_seconds.set({}, max(time.time() - boot_time, 0))


def _metrics_scope() -> str:
    configured_scope = config.monitoring.host_metrics_scope
    if configured_scope != "auto":
        return configured_scope
    if os.path.exists("/.dockerenv") or os.path.exists("/run/.containerenv") or os.environ.get("container"):
        return "container"
    return "host"


def collect_host_metrics() -> None:
    """Refresh portable host and Turbacz-process metrics for one scrape.

    No privileged operation is performed. Metrics unavailable to the current
    user or unsupported by an operating system are left out, while core
    collection errors are reflected in ``collection_success`` and logged.
    """
    started = time.monotonic()
    success = True
    try:
        hostname = socket.gethostname()
        host_info.set(
            {
                "hostname": hostname,
                "system": platform.system() or "unknown",
                "release": platform.release() or "unknown",
                "machine": platform.machine() or "unknown",
                "scope": _metrics_scope(),
            },
            1,
        )

        _set_uptime_metrics()

        # Keep categories isolated. For example, a protected mount point or a
        # platform-specific network API must not suppress CPU and memory data.
        for name, collector in (
            ("CPU", _set_cpu_metrics),
            ("memory", _set_memory_metrics),
            ("disk", _set_disk_metrics),
            ("network", _set_network_metrics),
            ("process", _set_process_metrics),
        ):
            try:
                collector()
            except Exception:
                success = False
                logger.exception("Could not collect %s performance metrics", name)

        _set_temperature_metrics()
    except Exception:
        success = False
        logger.exception("Could not collect host performance metrics")
    finally:
        host_metrics_collection_success.set({}, int(success))
        host_metrics_collection_duration_seconds.set({}, time.monotonic() - started)
