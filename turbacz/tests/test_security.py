import asyncio
import importlib
import threading
from datetime import UTC, datetime, timedelta
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

import jwt
import pytest
from fastapi import HTTPException
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect
from turbacz.database import db_call, serialized_manager
from turbacz.line_protocol import node_metrics
from turbacz.security import RateLimiter, csrf_token
from turbacz.settings import SecuritySettings, config
from turbacz.temperature_history import merge_temperature_series
from turbacz.validation import validate_command
from turbacz.websocket import ConnectionManager


@pytest.fixture
def application(monkeypatch, tmp_path, cm, client):
    monkeypatch.setattr(
        config, "jwt_secret", "test-only-jwt-key-with-at-least-48-bytes-for-all-tests"
    )
    monkeypatch.setattr(config, "session_secret", "test-only-session-key")
    monkeypatch.setattr(config, "authorized", {"test@example.com"})
    monkeypatch.setattr(
        config,
        "security",
        SecuritySettings(
            allowed_hosts=["testserver"],
            allowed_origins=["https://testserver"],
        ),
    )
    monkeypatch.setattr(config.oidc, "redirect_uri", "https://testserver/auth")
    monkeypatch.setattr(config.monitoring, "sentry_dsn", None)
    monkeypatch.setattr(config.monitoring, "collect_host_metrics", False)
    monkeypatch.setattr(config.firmware, "directory", str(tmp_path / "firmware"))
    # Import the real routes without opening PostgreSQL, MQTT or any firmware
    # files from the operator's installation. TestClient does not run lifespan.
    (tmp_path / "static").mkdir()
    monkeypatch.chdir(tmp_path)
    connection = MagicMock()
    cursor = connection.cursor.return_value.__enter__.return_value
    cursor.execute.return_value = cursor
    cursor.fetchall.return_value = []
    cursor.fetchone.return_value = [1]
    with patch("psycopg.connect", return_value=connection):
        main = importlib.import_module("turbacz.main")
    from turbacz import auth, connection_manager

    monkeypatch.setattr(auth, "JWT_SECRET", config.jwt_secret)
    monkeypatch.setattr(main, "connection_manager", cm)
    monkeypatch.setattr(connection_manager, "connection_manager", cm)
    monkeypatch.setattr(main.mqtt, "client", client)
    monkeypatch.setattr(main, "FIRMWARE_DIR", tmp_path / "firmware")
    # Rebuilding MetricsMiddleware would register duplicate collectors.
    from turbacz.security import SecurityMiddleware

    middleware = main.app.middleware_stack
    while middleware is not None:
        if isinstance(middleware, SecurityMiddleware):
            middleware.limiter = RateLimiter()
            middleware.uploading.clear()
        middleware = getattr(middleware, "app", None)
    main.ws_manager.active_connections.clear()
    main.ws_manager.clients.clear()
    main.ws_manager.send_locks.clear()
    main.ws_manager.limiter = RateLimiter()
    token = auth.create_jwt({"sub": "test@example.com", "name": "Test"})
    http = TestClient(main.app, base_url="https://testserver")
    return SimpleNamespace(
        main=main, auth=auth, http=http, token=token, cm=cm, mqtt=client
    )


def test_http_errors_and_authentication_status(application):
    a = application
    assert a.http.get("/missing").status_code == 404
    assert a.http.get("/lights/get_outputs").status_code == 401
    assert (
        a.http.post("/setblind", json={"blind": "r1", "position": 50}).status_code
        == 401
    )
    response = a.http.get(
        "/lights/get_outputs", headers={"Authorization": f"Bearer {a.token}"}
    )
    assert response.status_code == 200
    assert response.json()["111"]["a"][0] == "Ceiling"


async def test_exception_headers_are_preserved(application):
    response = await application.main.custom_http_exception_handler(
        None, HTTPException(401, "Unauthorized", headers={"WWW-Authenticate": "Bearer"})
    )
    assert response.status_code == 401
    assert response.headers["www-authenticate"] == "Bearer"


def test_csrf_cookie_bearer_origin_and_host_boundaries(application):
    a = application
    a.http.cookies.set("access_token", a.token)
    body = {"blind": "r1", "position": 50}
    assert a.http.post("/setblind", json=body).status_code == 403
    token_response = a.http.get("/csrf-token")
    assert token_response.headers["cache-control"] == "no-store"
    csrf = token_response.json()["token"]
    headers = {"Origin": "https://testserver", "X-CSRF-Token": csrf}
    assert a.http.post("/setblind", json=body, headers=headers).status_code == 200
    for origin in [
        "https://sibling.testserver",
        "https://evil.example",
        "null",
        "http://testserver",
    ]:
        assert (
            a.http.post(
                "/setblind", json=body, headers={**headers, "Origin": origin}
            ).status_code
            == 403
        )
    assert (
        a.http.post(
            "/setblind",
            json=body,
            headers={**headers, "X-CSRF-Token": csrf_token("other-token")},
        ).status_code
        == 403
    )
    assert (
        a.http.post(
            "/setblind", json=body, headers={"Authorization": f"Bearer {a.token}"}
        ).status_code
        == 200
    )
    assert (
        a.http.post(
            "/setblind", json=body, headers={"Authorization": "Bearer invalid"}
        ).status_code
        == 401
    )
    assert (
        a.http.get(
            "/", headers={"Host": "evil.example", "X-Forwarded-Host": "testserver"}
        ).status_code
        == 400
    )
    assert (
        a.http.get(
            "/", headers=[("Host", "testserver"), ("Host", "evil.example")]
        ).status_code
        == 400
    )
    assert a.http.get("/logout").status_code == 405
    assert (
        a.http.post("/logout", headers=headers, follow_redirects=False).status_code
        == 303
    )


def test_security_headers_and_secure_cookies_behind_http_proxy(
    application, monkeypatch
):
    a = application

    async def authorize(_request, **_kwargs):
        return {"userinfo": {"email": "test@example.com"}}

    monkeypatch.setattr(a.auth.oauth.google, "authorize_access_token", authorize)
    http = TestClient(a.main.app, base_url="http://testserver")
    response = http.get("/auth", follow_redirects=False)
    assert response.status_code == 307
    assert "Secure" in response.headers["set-cookie"]
    assert "HttpOnly" in response.headers["set-cookie"]
    assert response.headers["x-frame-options"] == "DENY"
    assert response.headers["x-content-type-options"] == "nosniff"
    assert "frame-ancestors 'none'" in response.headers["content-security-policy"]
    assert response.headers["strict-transport-security"] == "max-age=31536000"

    async def redirect(request, _uri):
        # Exercise SessionMiddleware cookie creation through the login route.
        from starlette.responses import RedirectResponse

        return RedirectResponse("https://accounts.google.com/")

    monkeypatch.setattr(a.auth.oauth.google, "authorize_redirect", redirect)
    response = http.get("/login", follow_redirects=False)
    assert "session=" in response.headers["set-cookie"]
    assert "secure" in response.headers["set-cookie"].lower()


def test_jwt_compatibility_and_invalid_tokens(application):
    a = application
    # Legacy python-jose HS256 tokens use the same claims/wire format.
    claims = {
        "sub": "test@example.com",
        "iat": int(datetime.now(UTC).timestamp()),
        "exp": int((datetime.now(UTC) + timedelta(hours=1)).timestamp()),
    }
    assert (
        a.auth.verify_jwt(jwt.encode(claims, a.auth.JWT_SECRET, algorithm="HS256"))[
            "sub"
        ]
        == claims["sub"]
    )
    assert a.auth.verify_jwt(a.token + "tamper") is None
    assert (
        a.auth.verify_jwt(
            jwt.encode({**claims, "exp": 0}, a.auth.JWT_SECRET, algorithm="HS256")
        )
        is None
    )
    assert (
        a.auth.verify_jwt(jwt.encode(claims, a.auth.JWT_SECRET, algorithm="HS384"))
        is None
    )
    assert (
        a.auth.verify_jwt(
            jwt.encode({"sub": claims["sub"]}, a.auth.JWT_SECRET, algorithm="HS256")
        )
        is None
    )


def test_explicit_local_http_opt_in(application, monkeypatch):
    from fastapi import FastAPI
    from starlette.middleware.sessions import SessionMiddleware
    from turbacz.security import SecurityMiddleware

    a = application
    monkeypatch.setattr(config.security, "allow_insecure_http", True)
    monkeypatch.setattr(config.security, "allowed_origins", ["http://testserver"])
    monkeypatch.setattr(config.oidc, "redirect_uri", "http://testserver/auth")
    app = FastAPI()
    app.include_router(a.auth.router)
    app.add_middleware(
        SessionMiddleware, secret_key=config.session_secret, https_only=False
    )
    app.add_middleware(SecurityMiddleware)

    async def authorize(_request, **_kwargs):
        return {"userinfo": {"email": "test@example.com"}}

    monkeypatch.setattr(a.auth.oauth.google, "authorize_access_token", authorize)
    response = TestClient(app, base_url="http://testserver").get(
        "/auth", follow_redirects=False
    )
    assert response.status_code == 307
    assert "secure" not in response.headers["set-cookie"].lower()
    assert "strict-transport-security" not in response.headers


@pytest.mark.parametrize(
    "path,initial",
    [
        ("/lights/ws/1", "configuration"),
        ("/blinds/ws/1", "relay_blinds"),
        ("/rcm/ws/1", "online_status"),
    ],
)
def test_all_websocket_routes_cleanup_and_valid_controls(application, path, initial):
    a = application
    command = {"relay_id": "111", "output_id": "a", "state": 1}
    expected = [("/relay/cmd/111", "a1", False)]
    if path.startswith("/blinds/"):
        command = {
            "type": "relay_blind_control",
            "relay_id": 111,
            "power_id": "c",
            "direction_id": "d",
            "action": "up",
        }
        expected = [("/relay/cmd/111", "d0", False), ("/relay/cmd/111", "c1", False)]
    with a.http.websocket_connect(f"{path}?token={a.token}") as ws:
        assert ws.receive_json()["type"] == initial
        # Ping follows initial state and lets the test observe completion of
        # earlier publication without sleeping.
        ws.send_json({"type": "ping"})
        while ws.receive_json().get("type") != "pong":
            pass
        a.mqtt.drain()
        ws.send_json(command)
        ws.send_json({"type": "ping"})
        assert ws.receive_json()["type"] == "pong"
        assert a.mqtt.drain() == expected
    assert not a.main.ws_manager.clients
    assert not a.main.ws_manager.active_connections


def test_start_applies_transport_frame_limits(application, monkeypatch):
    import uvicorn

    run = MagicMock()
    monkeypatch.setattr(uvicorn, "run", run)
    application.main.start()
    assert run.call_args.kwargs["ws"] == "websockets"
    assert run.call_args.kwargs["ws_max_size"] == config.security.ws_max_bytes
    assert run.call_args.kwargs["ws_max_queue"] == 16
    assert run.call_args.kwargs["ws_per_message_deflate"] is False


def test_auth_rate_limit_and_upload_auth_before_multipart(application, monkeypatch):
    a = application
    monkeypatch.setattr(config.security, "auth_requests_per_minute", 2)

    async def rejected(*_args, **_kwargs):
        from authlib.integrations.starlette_client import OAuthError

        raise OAuthError(error="access_denied")

    monkeypatch.setattr(a.auth.oauth.google, "authorize_access_token", rejected)
    assert a.http.get("/auth").status_code == 200
    assert a.http.get("/auth").status_code == 200
    response = a.http.get("/auth")
    assert response.status_code == 429
    assert response.headers["retry-after"] == "60"
    assert a.http.post("/upload/relay", content=b"broken multipart").status_code == 401
    monkeypatch.setattr(config.security, "uploads_per_minute", 1)
    headers = {"Authorization": f"Bearer {a.token}"}
    assert a.http.post("/upload/relay", headers=headers).status_code == 422
    assert a.http.post("/upload/relay", headers=headers).status_code == 429


def test_limiter_recovers_and_bounds_memory():
    clock = [0]
    limiter = RateLimiter(max_keys=2, clock=lambda: clock[0])
    assert limiter.allow("one", 1)
    assert not limiter.allow("one", 1)
    assert limiter.allow("two", 1)
    assert not limiter.allow("three", 1)
    assert len(limiter.entries) == 2
    clock[0] = 61
    assert limiter.allow("three", 1)
    assert limiter.allow("one", 1)


def test_websocket_browser_and_native_auth_cleanup(application):
    a = application
    a.http.cookies.set("access_token", a.token)
    for headers in [
        {},
        {"Origin": "https://evil.example"},
        {"Origin": "https://sibling.testserver"},
    ]:
        with (
            pytest.raises(WebSocketDisconnect),
            a.http.websocket_connect("/heating/ws/1", headers=headers),
        ):
            pass
    for path, headers in [
        ("/heating/ws/1", {"Origin": "https://testserver"}),
        (f"/heating/ws/1?token={a.token}", {}),
    ]:
        for _ in range(10):
            with a.http.websocket_connect(path, headers=headers) as ws:
                ws.send_json({"type": "ping"})
                assert ws.receive_json() == {"type": "pong"}
        assert not a.main.ws_manager.clients
        assert not a.main.ws_manager.active_connections


@pytest.mark.parametrize(
    "message,code",
    [
        ([], 1008),
        ({"type": "unknown"}, 1008),
        ("x" * 66000, 1009),
        ({"x": list(range(1100))}, 1008),
    ],
)
def test_websocket_rejects_invalid_and_oversized_messages(application, message, code):
    a = application
    with a.http.websocket_connect(f"/heating/ws/1?token={a.token}") as ws:
        ws.send_json(message)
        with pytest.raises(WebSocketDisconnect) as error:
            ws.receive_json()
        assert error.value.code == code
    assert not a.main.ws_manager.clients
    assert not a.mqtt.sent


def test_websocket_rate_connection_and_idle_limits(application, monkeypatch):
    a = application
    monkeypatch.setattr(config.security, "ws_connections_per_user", 1)
    monkeypatch.setattr(config.security, "ws_messages_per_minute", 2)
    path = f"/heating/ws/1?token={a.token}"
    with a.http.websocket_connect(path) as first:
        with pytest.raises(WebSocketDisconnect), a.http.websocket_connect(path):
            pass
        for _ in range(2):
            first.send_json({"type": "ping"})
            assert first.receive_json() == {"type": "pong"}
        first.send_json("t40")
        with pytest.raises(WebSocketDisconnect) as error:
            first.receive_json()
        assert error.value.code == 1013
    monkeypatch.setattr(config.security, "ws_idle_seconds", 0.02)
    with a.http.websocket_connect(path) as ws:
        with pytest.raises(WebSocketDisconnect) as error:
            ws.receive_json()
        assert error.value.code == 1001
    assert not a.main.ws_manager.clients


def test_failed_websocket_authentication_is_rate_limited(application, monkeypatch):
    a = application
    monkeypatch.setattr(config.security, "ws_handshakes_per_minute", 1)
    for expected in (1008, 1013):
        with pytest.raises(WebSocketDisconnect) as error, a.http.websocket_connect("/heating/ws/1?token=invalid"):
            pass
        assert error.value.code == expected
    assert not a.main.ws_manager.clients


async def test_slow_websocket_does_not_block_other_broadcasts(monkeypatch):
    monkeypatch.setattr(config.security, "ws_send_timeout", 0.02)
    manager = ConnectionManager()

    class Socket:
        url = SimpleNamespace(path="/heating/ws/1")
        client = SimpleNamespace(host="test")

        def __init__(self, slow):
            self.slow, self.sent = slow, []

        async def accept(self):
            pass

        async def close(self, **_kwargs):
            pass

        async def send_json(self, data):
            if self.slow:
                await asyncio.sleep(10)
            self.sent.append(data)

    slow, fast = Socket(True), Socket(False)
    await manager.connect(slow, "slow")
    await manager.connect(fast, "fast")
    await manager.broadcast({"cold": 10}, "/heating/ws/")
    assert fast.sent == [{"cold": 10}]
    assert slow not in manager.active_connections
    assert fast in manager.active_connections


@pytest.mark.parametrize(
    "command",
    [
        {"relay_id": "111/../../switch", "output_id": "a", "state": 1},
        {"relay_id": 111, "output_id": "a/#", "state": 1},
        {"relay_id": 111, "output_id": "p", "state": 1},
        {"relay_id": 111, "output_id": "a", "state": "01"},
        {
            "type": "update_device",
            "device_type": "relay/../../switch",
            "device_id": 111,
        },
        {"type": "update_device", "device_type": "relay", "device_id": "+"},
    ],
)
def test_invalid_commands_do_not_publish(application, command):
    a = application
    with a.http.websocket_connect(f"/rcm/ws/1?token={a.token}") as ws:
        # Initial state requests are legitimate; only inspect later publishes.
        while True:
            initial = ws.receive_json()
            if initial.get("type") == "online_status":
                break
        a.mqtt.drain()
        ws.send_json(command)
        with pytest.raises(WebSocketDisconnect):
            ws.receive_json()
    assert not a.mqtt.sent


def test_commands_preserve_existing_wire_format_and_validate_whole_batch(cm):
    validate_command(
        "/lights/ws/1", {"relay_id": "111", "output_id": "a", "state": 1}, cm
    )
    validate_command(
        "/blinds/ws/1",
        {
            "type": "relay_blind_control",
            "relay_id": 111,
            "power_id": "c",
            "direction_id": "d",
            "action": "up",
        },
        cm,
    )
    validate_command(
        "/rcm/ws/1", {"type": "button_types", "data": {"222": {"a": 1}}}, cm
    )
    with pytest.raises(ValueError):
        validate_command(
            "/lights/ws/1",
            {
                "type": "change_positions",
                "positions": [
                    {"relay_id": 111, "output_id": "a", "output_idx": 0},
                    {"relay_id": 111, "output_id": "a/#", "output_idx": 1},
                ],
            },
            cm,
        )


def test_form_output_ids_and_membership(application):
    a = application
    headers = {"Authorization": f"Bearer {a.token}"}
    for invalid in ["a' onclick='alert(1)", "aa", "a\n", "/#", "z"]:
        response = a.http.post(
            "/lights/add_blind_pair",
            data={
                "relay_id": 111,
                "output_id_power": invalid,
                "output_id_direction": "b",
            },
            headers=headers,
        )
        assert response.status_code == 422
    response = a.http.post(
        "/lights/add_blind_pair",
        data={"relay_id": 111, "output_id_power": "p", "output_id_direction": "b"},
        headers=headers,
    )
    assert response.status_code == 400


def test_metrics_escape_all_tags_and_reject_injected_fields():
    data = {
        "deviceId": 111,
        "name": "A, B=C",
        "parentId": 222,
        "firmware": "a,b=c",
        "type": "relay8",
        "uptime": 1,
        "clicks": 2,
        "freeHeap": 100,
        "rssi": -40,
    }
    node, mesh = node_metrics(data, "P, Q=R", {"site,name": "a=b c"}, 5)
    assert "name=A\\,\\ B\\=C" in node
    assert "site\\,name=a\\=b\\ c" in mesh
    assert "parent_name=P\\,\\ Q\\=R" in mesh
    for field, value in [
        ("firmware", "x\ninjected value=1"),
        ("name", "x\rfoo"),
        ("clicks", "1,evil=2"),
        ("rssi", float("nan")),
        ("freeHeap", float("inf")),
    ]:
        with pytest.raises(ValueError):
            node_metrics({**data, field: value}, "parent", {}, 5)


def test_temperature_history_joins_labels_and_sparse_timestamps():
    water = [
        {"metric": {"probe": "hot"}, "values": [[4, "50"]]},
        {"metric": {"probe": "cold"}, "values": [[0, "10"], [4, "NaN"]]},
    ]
    target = [{"metric": {}, "values": [[0, "40"]]}]
    assert merge_temperature_series(water, target) == [
        {"timestamp": 0, "cold": 10, "hot": None, "mixed": None, "target": 40},
        {"timestamp": 4, "cold": None, "hot": 50, "mixed": None, "target": None},
    ]


async def test_database_work_is_off_loop_and_transactions_do_not_interleave():
    entered = threading.Event()
    release = threading.Event()
    events = []

    @serialized_manager
    class Manager:
        def __init__(self):
            self._db_lock = threading.RLock()
            self.conn = MagicMock()

        def write(self, value):
            events.append((value, "start"))
            if value == 1:
                entered.set()
                release.wait(1)
            events.append((value, "commit"))

    manager = Manager()
    first = asyncio.create_task(db_call(manager.write, 1))
    await asyncio.to_thread(entered.wait, 1)
    second = asyncio.create_task(db_call(manager.write, 2))
    await asyncio.sleep(0.01)
    assert events == [(1, "start")]
    release.set()
    await asyncio.gather(first, second)
    assert events == [(1, "start"), (1, "commit"), (2, "start"), (2, "commit")]
