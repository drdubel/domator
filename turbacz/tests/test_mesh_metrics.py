import pytest
from aioprometheus import REGISTRY, render

from turbacz import mesh_metrics as mesh
from turbacz.settings import config


@pytest.fixture(autouse=True)
def clear_mesh():
    mesh._snapshots.clear()
    mesh.collect_mesh_metrics()
    yield
    mesh._snapshots.clear()
    mesh.collect_mesh_metrics()


def report(**changes):
    return {
        "deviceId": 111,
        "name": 'Kitchen "lamp"',
        "parentId": 222,
        "firmware": 1,
        "type": "relay8",
        "uptime": 100,
        "clicks": 5,
        "freeHeap": 20000,
        "rssi": -50,
        **changes,
    }


def exposition():
    mesh.collect_mesh_metrics()
    return render(REGISTRY, ["text/plain"])[0].decode()


def test_names_labels_and_optional_ping(monkeypatch):
    monkeypatch.setattr(config.monitoring, "labels", {"apartment": "flat1"})
    mesh.record_node_metrics(report(), "Root", None)
    text = exposition()
    for name in (
        "node_info_uptime",
        "node_info_clicks",
        "node_info_free_heap",
        "mesh_node_rssi",
    ):
        assert f"{name}{{" in text
    assert 'apartment="flat1"' in text
    assert 'name="Kitchen \\"lamp\\""' in text
    assert 'parent="222"' in text
    assert "node_info_ping_time{" not in text
    mesh.record_node_metrics(report(clicks=0, uptime=1), "Root", 12)
    exposition()
    snapshot = mesh._snapshots["111"]
    assert mesh.readings["clicks"].get(snapshot.node_labels) == 0
    assert mesh.readings["ping"].get(snapshot.node_labels) == 12


def test_stale_readings_expire_and_offline_devices_are_forgotten(monkeypatch):
    clock = [100]
    monkeypatch.setattr(mesh.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(mesh.time, "time", lambda: 1700000000)
    mesh.record_node_metrics(report(), "Root", 5)
    labels = mesh._snapshots["111"].node_labels
    exposition()
    assert mesh.available.get(labels) == 1
    clock[0] += 31
    text = exposition()
    assert "mesh_node_rssi{" not in text
    assert "node_info_uptime{" not in text
    assert mesh.available.get(labels) == 0
    assert mesh.last_seen.get(labels) == 1700000000
    mesh.record_node_metrics(report(), "Root", 5)
    exposition()
    assert mesh.available.get(labels) == 1
    clock[0] += 3601
    text = exposition()
    assert not mesh._snapshots
    assert "node_info_last_seen_seconds{" not in text


def test_relabel_and_missing_ping_remove_previous_series():
    mesh.record_node_metrics(report(), "Root", 5)
    exposition()
    mesh.record_node_metrics(
        report(name="New", parentId=333, firmware=2), "Other", None
    )
    text = exposition()
    assert 'parent="222"' not in text
    assert 'parent="333"' in text
    assert 'firmware="1"' not in text
    assert "node_info_ping_time{" not in text
    assert len(mesh._snapshots) == 1


def test_invalid_report_does_not_replace_good_snapshot():
    mesh.record_node_metrics(report(), "Root", 5)
    before = mesh._snapshots["111"]
    with pytest.raises(ValueError):
        mesh.record_node_metrics(report(rssi=float("nan")), "Root", 5)
    assert mesh._snapshots["111"] is before
