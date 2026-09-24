import asyncio
import json
from threading import RLock
from types import SimpleNamespace
from unittest.mock import AsyncMock, MagicMock

import httpx
import pytest

from turbacz import metrics_client
from turbacz.settings import config

from .test_security import (
    application as application,  # noqa: PLC0414 -- shared pytest fixture
)


async def test_shared_pool_batches_reports_and_serves_history(application, monkeypatch):
    from turbacz import broker

    requests = []

    def respond(request):
        requests.append(request)
        if request.method == "POST":
            return httpx.Response(204)
        return httpx.Response(200, json={"data": {"result": []}})

    pool = httpx.AsyncClient(transport=httpx.MockTransport(respond))
    factory = MagicMock(return_value=pool)
    monkeypatch.setattr(metrics_client.httpx, "AsyncClient", factory)
    monkeypatch.setattr(application.main.mqtt, "mqtt_startup", AsyncMock())
    monkeypatch.setattr(application.main.mqtt, "mqtt_shutdown", AsyncMock())
    monkeypatch.setattr(broker, "connection_manager", application.cm)
    monkeypatch.setattr(broker, "state_manager", MagicMock())
    broker.state_manager.get_device_ping.return_value = 5
    monkeypatch.setattr(broker, "ha_bridge", MagicMock())
    broker.ha_bridge._resync_task = None
    monkeypatch.setattr(config.monitoring, "send_metrics", True)
    data = {
        "deviceId": 111,
        "parentId": 111,
        "type": "relay8",
        "firmware": 1,
        "uptime": 100,
        "clicks": 0,
        "freeHeap": 20000,
        "rssi": -50,
    }

    async with application.main.lifespan(application.main.app):
        for _ in range(3):
            await broker.handle_root_state(json.dumps(data))
        assert await application.main.get_temperatures(None, 0, 3600, 4) == []
        assert not pool.is_closed
        factory.assert_called_once()
    assert pool.is_closed
    with pytest.raises(RuntimeError):
        metrics_client.get_metrics_client()
    writes = [r for r in requests if r.method == "POST"]
    assert len(writes) == 3
    for request in writes:
        lines = request.content.decode().splitlines()
        assert len(lines) == 2
        assert lines[0].startswith("node_info,")
        assert lines[1].startswith("mesh_node,")
    assert len(requests) == 5


async def test_pool_closes_when_mqtt_startup_fails(application, monkeypatch):
    pool = httpx.AsyncClient(
        transport=httpx.MockTransport(lambda r: httpx.Response(204))
    )
    monkeypatch.setattr(metrics_client.httpx, "AsyncClient", lambda: pool)
    monkeypatch.setattr(
        application.main.mqtt, "mqtt_startup", AsyncMock(side_effect=OSError("offline"))
    )
    shutdown = AsyncMock()
    monkeypatch.setattr(application.main.mqtt, "mqtt_shutdown", shutdown)
    with pytest.raises(OSError, match="offline"):
        async with application.main.lifespan(application.main.app):
            pytest.fail("startup should fail")
    assert pool.is_closed
    shutdown.assert_awaited_once()


async def test_reconnects_keep_one_pair_of_background_tasks(application, monkeypatch):
    from turbacz import broker

    async def forever():
        await asyncio.Event().wait()

    bridge = SimpleNamespace(
        _resync_task=None,
        reset_published_state=MagicMock(),
        command_subscriptions=list,
        schedule_resync=MagicMock(),
        resync_loop=AsyncMock(side_effect=forever),
    )
    check = AsyncMock(side_effect=forever)
    monkeypatch.setattr(broker, "ha_bridge", bridge)
    monkeypatch.setattr(broker, "periodic_check_devices", check)
    monkeypatch.setattr(broker.mqtt, "client", MagicMock())
    try:
        for _ in range(10):
            broker.connect(None, None, 0, None)
            await asyncio.sleep(0)
        check.assert_awaited_once()
        bridge.resync_loop.assert_awaited_once()
        assert bridge.schedule_resync.call_count == 9
        tasks = (broker._device_task, broker._ha_task)
    finally:
        await broker.stop_background_tasks()
    assert all(task.cancelled() for task in tasks)
    assert broker._device_task is broker._ha_task is None


def test_registry_cache_invalidates_on_edits_and_expires(application, monkeypatch):
    from turbacz import database
    from turbacz.connection_manager import ConnectionManager

    cm = ConnectionManager.__new__(ConnectionManager)
    cm._db_lock = RLock()
    cm._registry_cache = {}
    cm.conn = MagicMock()
    cursor = cm.conn.cursor.return_value.__enter__.return_value
    cursor.fetchall.return_value = [(111, "Before", 8)]
    clock = [0]
    monkeypatch.setattr(database, "monotonic", lambda: clock[0])
    first = cm.get_relays()
    first.clear()
    assert cm.get_relays() == {111: ("Before", 8)}
    assert cursor.fetchall.call_count == 1
    cm.rename_relay(111, "After", 8)
    cursor.fetchall.return_value = [(111, "After", 8)]
    assert cm.get_relays() == {111: ("After", 8)}
    assert cursor.fetchall.call_count == 2
    cursor.fetchall.return_value = [(111, "External edit", 8)]
    clock[0] = 61
    assert cm.get_relays() == {111: ("External edit", 8)}


async def test_unchanged_relay_reports_skip_websocket_fanout(application, monkeypatch):
    from turbacz import state_manager

    broadcasts = AsyncMock()
    ha = AsyncMock()
    monkeypatch.setattr(state_manager.ws_manager, "broadcast", broadcasts)
    monkeypatch.setattr(state_manager.ha_bridge, "on_relay_state", ha)
    state = state_manager.StateManager()
    await state.update_state(111, "a", 1)
    assert broadcasts.await_count == 3
    await state.update_state(111, "a", 1)
    assert broadcasts.await_count == 3
    assert ha.await_count == 2
    await state.update_state(111, "a", 0)
    assert broadcasts.await_count == 6


async def test_history_rejects_excessive_work_before_querying(application):
    from fastapi import HTTPException

    for start, end in [(0, 10000), (2, 1)]:
        with pytest.raises(HTTPException) as error:
            await application.main.get_temperatures(None, start, end, 1)
        assert error.value.status_code == 400
