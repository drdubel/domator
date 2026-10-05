from unittest.mock import AsyncMock

import httpx2
import pytest

from turbacz.body_limit import CustomRequestSizeMiddleware
from turbacz.settings import TurbaczSettings, config
from .test_security import application as application


@pytest.mark.parametrize(
    "headers", [[], [(b"content-length", b"1")], [(b"transfer-encoding", b"chunked")]]
)
async def test_body_limit_counts_stream_bytes(headers):
    reached = []

    async def app(scope, receive, send):
        while (await receive()).get("more_body"):
            pass
        reached.append(True)

    chunks = iter(
        [
            {"type": "http.request", "body": b"1234", "more_body": True},
            {"type": "http.request", "body": b"5678", "more_body": False},
        ]
    )

    async def receive():
        return next(chunks)

    messages = []

    async def send(message):
        messages.append(message)

    await CustomRequestSizeMiddleware(app, 7)(
        {"type": "http", "headers": headers}, receive, send
    )
    assert not reached
    assert messages[0]["status"] == 413


@pytest.mark.parametrize("value", [b"-1", b"abc", b"1.0", b"+1"])
async def test_invalid_content_length_rejected(value):
    app, send = AsyncMock(), AsyncMock()
    await CustomRequestSizeMiddleware(app, 7)(
        {"type": "http", "headers": [(b"content-length", value)]}, AsyncMock(), send
    )
    assert send.call_args_list[0].args[0]["status"] == 400


def test_settings_precedence_and_nested_secrets(monkeypatch, tmp_path):
    toml = tmp_path / "config.toml"
    toml.write_text(
        'jwt_secret = "toml"\n[mqtt]\npassword = "toml-mqtt"\nhost = "toml-host"\n'
    )
    secrets = tmp_path / "secrets"
    secrets.mkdir()
    (secrets / "TURBACZ_JWT_SECRET").write_text("file-secret")
    (secrets / "TURBACZ_MQTT__PASSWORD").write_text("file-mqtt")
    monkeypatch.setenv("TURBACZ_CONFIG_PATH", str(toml))
    monkeypatch.setenv("TURBACZ_SECRETS_DIR", str(secrets))
    monkeypatch.delenv("TURBACZ_JWT_SECRET", raising=False)
    settings = TurbaczSettings()
    assert settings.jwt_secret == "file-secret"
    assert settings.mqtt.password == "file-mqtt"
    assert settings.mqtt.host == "toml-host"
    monkeypatch.setenv("TURBACZ_JWT_SECRET", "environment")
    assert TurbaczSettings().jwt_secret == "environment"
    assert TurbaczSettings(jwt_secret="explicit").jwt_secret == "explicit"
    dotenv = tmp_path / ".env"
    dotenv.write_text("TURBACZ_JWT_SECRET=dotenv\n")
    monkeypatch.delenv("TURBACZ_JWT_SECRET")
    assert TurbaczSettings(_env_file=dotenv).jwt_secret == "dotenv"


@pytest.mark.parametrize(
    "topic,payload",
    [
        ("/relay/state/111", b""),
        ("/relay/state/111", b"A"),
        ("/relay/state/111", b"A2"),
        ("/relay/state/111", b"\xff"),
        ("/relay/state/111", b"A1extra"),
        ("/relay/state/nope", b"A1"),
        ("/switch/state/222", b"aX"),
        ("/switch/state/222", b"-1"),
        ("/switch/state/root", b"[]"),
        ("/switch/state/root", b"{}"),
        ("/switch/state/root", b'{"type":"relay8","deviceId":111}'),
        ("/heating/metrics", b'{"cold":NaN}'),
        ("/blind/pos", b"r9 50"),
        ("/blind/pos", b"r1 101"),
        ("/switch/state/root", b"[" * 2000),
        ("/switch/state/root", b"x" * 65537),
    ],
)
async def test_invalid_mqtt_has_no_side_effects(
    application, monkeypatch, topic, payload
):
    from turbacz import broker

    dispatch = AsyncMock()
    monkeypatch.setattr(broker, "dispatch_message", dispatch)
    await broker.message(None, topic, payload, 0, None)
    dispatch.assert_not_awaited()


async def test_mqtt_failure_isolation(application, monkeypatch):
    from turbacz import broker

    dispatch = AsyncMock(side_effect=RuntimeError("isolated failure"))
    monkeypatch.setattr(broker, "dispatch_message", dispatch)
    await broker.message(None, "/relay/state/111", b"A1", 0, None)
    dispatch.assert_awaited_once()


def test_upload_streaming_and_private_download(application):
    a = application
    headers = {"Authorization": f"Bearer {a.token}"}
    payload = b"\xe9" + b"x" * 200000
    response = a.http.post(
        "/upload/relay", files={"file": ("fw.bin", payload)}, headers=headers
    )
    assert response.status_code == 200
    assert response.json()["size"] == len(payload)
    assert (a.main.FIRMWARE_DIR / "relay.bin").read_bytes() == payload
    assert len(list(a.main.FIRMWARE_DIR.iterdir())) == 1
    assert (
        a.http.get("/firmware/relay.bin", headers=headers).headers["cache-control"]
        == "private, no-store"
    )
    response = a.http.post(
        "/upload/relay", files={"file": ("fw.bin", b"bad")}, headers=headers
    )
    assert response.status_code == 400
    assert (a.main.FIRMWARE_DIR / "relay.bin").read_bytes() == payload
    assert len(list(a.main.FIRMWARE_DIR.iterdir())) == 1


def test_large_upload_with_understated_length(application):
    a = application
    response = a.http.post(
        "/upload/relay",
        files={"file": ("fw.bin", b"\xe9" + b"x" * 10_000_001)},
        headers={"Authorization": f"Bearer {a.token}", "Content-Length": "1"},
    )
    assert response.status_code == 413
    assert not a.main.FIRMWARE_DIR.exists()


def test_telemetry_auth_and_ranges(application, monkeypatch):
    a = application
    assert a.http.get("/metrics").status_code == 401
    assert a.http.get("/api/temperatures?start=0&end=3600&step=4").status_code == 401
    monkeypatch.setattr(config.monitoring, "scrape_token", "scraper-secret")
    assert (
        a.http.get(
            "/metrics", headers={"Authorization": "Bearer scraper-secret"}
        ).status_code
        == 200
    )
    assert (
        a.http.get("/metrics", headers={"Authorization": "Bearer wrong"}).status_code
        == 401
    )
    headers = {"Authorization": f"Bearer {a.token}"}
    for query in [
        "start=0&end=86401&step=100",
        "start=5&end=4&step=4",
        "start=0&end=100&step=1",
    ]:
        assert a.http.get(
            "/api/temperatures?" + query, headers=headers
        ).status_code in (400, 422)


async def test_history_bounds_upstream_response(application, monkeypatch):
    a = application
    async with httpx2.AsyncClient(
        transport=httpx2.MockTransport(
            lambda r: httpx2.Response(200, content=b"x" * 4_000_001)
        )
    ) as client:
        monkeypatch.setattr(a.main, "get_metrics_client", lambda: client)
        response = a.http.get(
            "/api/temperatures?start=0&end=3600&step=4",
            headers={"Authorization": f"Bearer {a.token}"},
        )
    assert response.status_code == 502


def test_removed_user_token_is_rejected_on_http_and_live_socket(
    application, monkeypatch
):
    a = application
    assert a.auth.verify_jwt(a.token)
    with a.http.websocket_connect(f"/heating/ws/1?token={a.token}") as ws:
        monkeypatch.setattr(config, "authorized", set())
        assert a.auth.verify_jwt(a.token) is None
        assert (
            a.http.get(
                "/lights/get_outputs", headers={"Authorization": f"Bearer {a.token}"}
            ).status_code
            == 401
        )
        ws.send_json("t40")
        from starlette.websockets import WebSocketDisconnect

        with pytest.raises(WebSocketDisconnect):
            ws.receive_json()
    assert not a.mqtt.sent


@pytest.mark.parametrize("payload", ["nan", "inf", "-inf", "1e400", ""])
async def test_ha_rejects_nonfinite_heating_targets(bridge, client, payload):
    await bridge.apply()
    client.drain()
    await bridge.handle_command("domator/water_heater/heating/target/set", payload)
    assert not client.sent


@pytest.mark.parametrize(
    "role,capability,expected",
    [
        ("viewer", "viewer", True),
        ("viewer", "operator", False),
        ("viewer", "admin", False),
        ("viewer", "ota", False),
        ("operator", "viewer", True),
        ("operator", "operator", True),
        ("operator", "admin", False),
        ("operator", "ota", False),
        ("ota", "viewer", True),
        ("ota", "operator", False),
        ("ota", "admin", False),
        ("ota", "ota", True),
        ("admin", "viewer", True),
        ("admin", "operator", True),
        ("admin", "admin", True),
        ("admin", "ota", True),
    ],
)
def test_role_matrix(application, monkeypatch, role, capability, expected):
    from turbacz.authorization import permitted

    monkeypatch.setattr(config, "roles", {"test@example.com": {role}})
    assert (
        permitted(application.auth.verify_jwt(application.token), capability)
        is expected
    )


def test_all_topology_mutations_require_admin(application, monkeypatch):
    a = application
    monkeypatch.setattr(config, "roles", {"test@example.com": {"operator"}})
    paths = [
        route.path
        for route in a.main.connection_router.routes
        if "POST" in route.methods
    ]
    assert paths
    for path in paths:
        assert (
            a.http.post(
                path, headers={"Authorization": f"Bearer {a.token}"}
            ).status_code
            == 403
        )
    assert (
        a.http.post(
            "/upload/relay", headers={"Authorization": f"Bearer {a.token}"}
        ).status_code
        == 403
    )
    monkeypatch.setattr(config, "roles", {})
    assert (
        a.http.post(
            "/setblind",
            json={"blind": "r1", "position": 50},
            headers={"Authorization": f"Bearer {a.token}"},
        ).status_code
        == 403
    )


@pytest.mark.parametrize(
    "path,command,capability",
    [
        ("/heating/ws/1", "t40", "operator"),
        ("/lights/ws/1", {"relay_id": 111, "output_id": "a", "state": 1}, "operator"),
        ("/blinds/ws/1", {"type": "relay_blind_control"}, "operator"),
        ("/lights/ws/1", {"type": "add_section", "name": "New"}, "admin"),
        ("/lights/ws/1", {"type": "change_section"}, "admin"),
        ("/lights/ws/1", {"type": "change_positions"}, "admin"),
        ("/lights/ws/1", {"type": "layout_update"}, "admin"),
        ("/rcm/ws/1", {"type": "update_root"}, "ota"),
        ("/rcm/ws/1", {"type": "update_all_relays"}, "ota"),
        ("/rcm/ws/1", {"type": "update_all_switches"}, "ota"),
        ("/rcm/ws/1", {"type": "update_device"}, "ota"),
        ("/rcm/ws/1", {"type": "gateway_mode"}, "admin"),
        ("/rcm/ws/1", {"type": "button_types"}, "admin"),
        ("/rcm/ws/1", {"type": "auto_off_update"}, "admin"),
        ("/rcm/ws/1", {"type": "update"}, "admin"),
        ("/rcm/ws/1", {"type": "get_states"}, "viewer"),
    ],
)
def test_command_capabilities(path, command, capability):
    from turbacz.authorization import command_capability

    assert command_capability(path, command) == capability


def test_viewer_socket_cannot_publish_control(application, monkeypatch):
    a = application
    monkeypatch.setattr(config, "roles", {})
    from starlette.websockets import WebSocketDisconnect

    with a.http.websocket_connect(f"/heating/ws/1?token={a.token}") as ws:
        ws.send_json("t40")
        with pytest.raises(WebSocketDisconnect):
            ws.receive_json()
    assert not a.mqtt.sent


def test_setup_private_files_and_existing_config_protection(tmp_path):
    import os
    import subprocess
    from pathlib import Path

    script = Path(__file__).resolve().parents[1] / "setup.sh"
    tools = tmp_path / "tools"
    tools.mkdir()
    docker = tools / "docker"
    docker.write_text("#!/bin/sh\nexit 0\n")
    docker.chmod(0o755)
    env = {**os.environ, "PATH": str(tools) + os.pathsep + os.environ["PATH"]}
    result = subprocess.run(
        ["bash", str(script)],
        cwd=tmp_path,
        env=env,
        input="owner@example.com\ny\nn\nhttps://localhost\nplaceholder-mesh-password\n\n\n",
        text=True,
        capture_output=True,
    )
    assert result.returncode == 0
    for file in ["turbacz.toml", ".env", "monitoring/scrape_token", "mosquitto.passwd"]:
        assert (tmp_path / file).stat().st_mode & 0o777 == 0o600
    before = (tmp_path / "turbacz.toml").read_bytes()
    result = subprocess.run(
        ["bash", str(script)], cwd=tmp_path, env=env, capture_output=True
    )
    assert result.returncode == 1
    assert (tmp_path / "turbacz.toml").read_bytes() == before


def test_upload_rejects_extra_multipart_files(application):
    a = application
    response = a.http.post(
        "/upload/relay",
        files=[("file", ("fw.bin", b"\xe9x")), ("other", ("other.bin", b"x"))],
        headers={"Authorization": f"Bearer {a.token}"},
    )
    assert response.status_code == 400
    assert not a.main.FIRMWARE_DIR.exists()
