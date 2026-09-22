from types import SimpleNamespace

import turbacz.metrics as metrics


def test_memory_metrics_survive_unavailable_swap(monkeypatch):
    memory = SimpleNamespace(
        total=1000,
        available=600,
        used=400,
        free=500,
        active=300,
        inactive=200,
        buffers=10,
        cached=90,
        percent=40.0,
    )
    monkeypatch.setattr(metrics.psutil, "virtual_memory", lambda: memory)

    def unavailable_swap():
        raise OSError("swap information is restricted")

    monkeypatch.setattr(metrics.psutil, "swap_memory", unavailable_swap)

    metrics._set_memory_metrics()

    assert metrics.host_memory_bytes.get({"state": "total"}) == 1000
    assert metrics.host_memory_usage_percent.get({}) == 40.0


def test_collection_failure_does_not_hide_other_categories(monkeypatch):
    calls = []

    monkeypatch.setattr(metrics, "_set_uptime_metrics", lambda: None)
    monkeypatch.setattr(metrics, "_set_cpu_metrics", lambda: calls.append("cpu"))
    monkeypatch.setattr(metrics, "_set_memory_metrics", lambda: calls.append("memory"))

    def broken_disk():
        calls.append("disk")
        raise OSError("unavailable disk counters")

    monkeypatch.setattr(metrics, "_set_disk_metrics", broken_disk)
    monkeypatch.setattr(metrics, "_set_network_metrics", lambda: calls.append("network"))
    monkeypatch.setattr(metrics, "_set_process_metrics", lambda: calls.append("process"))
    monkeypatch.setattr(metrics, "_set_temperature_metrics", lambda: None)

    metrics.collect_host_metrics()

    assert calls == ["cpu", "memory", "disk", "network", "process"]
    assert metrics.host_metrics_collection_success.get({}) == 0
