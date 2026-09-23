from types import SimpleNamespace

import turbacz.metrics as metrics


def test_wifi_signal_and_disappearing_interfaces(tmp_path, monkeypatch):
    wireless = tmp_path / "wireless"
    monkeypatch.setenv("TURBACZ_WIFI_WIRELESS_PATH", str(wireless))
    wireless.write_text(
        "Inter-| sta-| Quality | Discarded packets\n"
        " wlan0: 0000 70. -45. -256 0 0 0\n"
        " wlan1: 0000 40. -72. -256 0 0 0\n"
        " invalid: broken\n"
        " relative: 0000 40. 50. 0 0 0\n"
        " disconnected: 0000 0. 0. 0 0 0\n"
    )
    metrics._set_wifi_metrics()
    assert metrics.host_wifi_signal_dbm.get({"interface": "wlan0"}) == -45
    assert metrics.host_wifi_signal_dbm.get({"interface": "wlan1"}) == -72
    assert len(metrics.host_wifi_signal_dbm.values) == 2
    wireless.write_text("Inter-| sta-| Quality\n")
    metrics._set_wifi_metrics()
    assert not metrics.host_wifi_signal_dbm.values


def test_wifi_missing_file_removes_previous_readings(tmp_path, monkeypatch):
    monkeypatch.setenv("TURBACZ_WIFI_WIRELESS_PATH", str(tmp_path / "missing"))
    metrics.host_wifi_signal_dbm.set({"interface": "wlan0"}, -50)
    metrics._set_wifi_metrics()
    assert not metrics.host_wifi_signal_dbm.values


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
