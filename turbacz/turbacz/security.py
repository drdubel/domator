"""HTTP/handshake boundaries shared by every route (single worker limits)."""

import hashlib
import hmac
import time
from urllib.parse import urlsplit

from starlette.requests import HTTPConnection
from starlette.responses import JSONResponse
from turbacz.settings import config


class RateLimiter:
    """Bounded fixed windows. Refuse new keys when full; never evict an active limit."""

    def __init__(self, max_keys=4096, clock=time.monotonic):
        self.entries = {}
        self.max_keys = max_keys
        self.clock = clock

    def allow(self, key, limit, seconds=60):
        now = self.clock()
        count, end = self.entries.get(key, (0, now + seconds))
        if now >= end:
            count, end = 0, now + seconds
        if key not in self.entries and len(self.entries) >= self.max_keys:
            self.entries = {k: v for k, v in self.entries.items() if v[1] > now}
            if len(self.entries) >= self.max_keys:
                return False
        if count >= limit:
            return False
        self.entries[key] = (count + 1, end)
        return True


def csrf_token(access_token: str) -> str:
    # Bound to the HttpOnly login cookie; a sibling origin cannot mint a token.
    return hmac.new(
        config.jwt_secret.encode(), b"csrf\0" + access_token.encode(), hashlib.sha256
    ).hexdigest()


def allowed_origins():
    origins = set(config.security.allowed_origins)
    if config.oidc.redirect_uri:
        url = urlsplit(config.oidc.redirect_uri)
        if url.scheme in {"https", "http"} and url.netloc:
            origins.add(f"{url.scheme}://{url.netloc}")
    return {
        o
        for o in origins
        if config.security.allow_insecure_http or o.startswith("https://")
    }


def trusted_host(connection):
    values = connection.headers.getlist("host")
    if len(values) != 1:
        return False
    try:
        value = values[0]
        if any(c.isspace() for c in value):
            return False
        url = urlsplit("//" + value)
        if (
            url.username
            or url.password
            or url.path
            or url.query
            or url.fragment
            or not url.hostname
        ):
            return False
        _ = url.port
        hosts = set(config.security.allowed_hosts)
        if config.oidc.redirect_uri:
            hosts.add(urlsplit(config.oidc.redirect_uri).hostname)
        return url.hostname in hosts
    except ValueError:
        return False


def trusted_origin(connection):
    values = connection.headers.getlist("origin")
    return len(values) == 1 and values[0] in allowed_origins()


def security_headers():
    # Legacy controls still use inline handlers; keep those working while
    # restricting script sources, framing, objects, base URLs and forms.
    connect = ["'self'"]
    for origin in allowed_origins():
        connect.append(origin.replace("https://", "wss://").replace("http://", "ws://"))
    if config.monitoring.sentry_dsn:
        dsn = urlsplit(config.monitoring.sentry_dsn)
        if dsn.scheme == "https" and dsn.hostname:
            connect.append(
                f"https://{dsn.hostname}" + (f":{dsn.port}" if dsn.port else "")
            )
    headers = {
        "X-Content-Type-Options": "nosniff",
        "X-Frame-Options": "DENY",
        "Referrer-Policy": "same-origin",
        "Content-Security-Policy": (
            "default-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; "
            "form-action 'self'; script-src 'self' 'unsafe-inline' https://cdn.jsdelivr.net "
            "https://code.jquery.com https://browser.sentry-cdn.com; "
            "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com https://code.jquery.com; "
            "font-src 'self' https://fonts.gstatic.com; img-src 'self' data:; connect-src "
            + " ".join(sorted(set(connect)))
            + ";"
        ),
    }
    if not config.security.allow_insecure_http:
        headers["Strict-Transport-Security"] = "max-age=31536000"
    return headers


class SecurityMiddleware:
    def __init__(self, app):
        self.app = app
        self.limiter = RateLimiter()
        self.uploading = set()

    async def __call__(self, scope, receive, send):
        if scope["type"] not in {"http", "websocket"}:
            return await self.app(scope, receive, send)
        connection = HTTPConnection(scope)
        ip = connection.client.host if connection.client else "unknown"

        async def secured_send(message):
            if message["type"] == "http.response.start":
                message["headers"] = list(message.get("headers", [])) + [
                    (k.lower().encode(), v.encode())
                    for k, v in security_headers().items()
                ]
            await send(message)

        async def reject(status, detail):
            if scope["type"] == "websocket":
                await send({"type": "websocket.close", "code": 1013 if status == 429 else 1008})
            else:
                headers = {"Retry-After": "60"} if status == 429 else None
                await JSONResponse(
                    {"detail": detail}, status_code=status, headers=headers
                )(scope, receive, secured_send)

        if not trusted_host(connection):
            return await reject(400, "Invalid Host")
        if scope["type"] == "websocket":
            if not self.limiter.allow(("ws-handshake", ip), config.security.ws_handshakes_per_minute):
                return await reject(429, "Too many WebSocket handshakes")
            # Browser handshakes always send Origin. Native clients can use
            # explicit credentials without an Origin; auth enforces that rule.
            if "origin" in connection.headers and not trusted_origin(connection):
                return await reject(403, "Invalid Origin")
            return await self.app(scope, receive, send)

        from turbacz import auth

        path = scope["path"]
        if path in {"/login", "/auth"} and not self.limiter.allow(
            ("auth", ip), config.security.auth_requests_per_minute
        ):
            return await reject(429, "Too many authentication requests")

        unsafe = scope["method"] not in {"GET", "HEAD", "OPTIONS"}
        if unsafe and "origin" in connection.headers and not trusted_origin(connection):
            return await reject(403, "Invalid Origin")
        cookie = connection.cookies.get("access_token")
        bearer = auth.bearer_token_from_header(connection.headers.get("authorization"))
        user = auth.get_current_user(bearer or cookie)
        if unsafe and cookie and not bearer:
            if not user:
                return await reject(401, "Unauthorized")
            supplied = connection.headers.get("x-csrf-token", "")
            if not trusted_origin(connection) or not hmac.compare_digest(
                supplied.encode(), csrf_token(cookie).encode()
            ):
                return await reject(403, "Invalid CSRF token or Origin")

        upload_key = None
        if unsafe and path.startswith("/upload/"):
            if not user:
                return await reject(401, "Unauthorized")
            upload_key = user["sub"]
            limit = config.security.uploads_per_minute
            if not self.limiter.allow(
                ("upload-ip", ip), limit
            ) or not self.limiter.allow(("upload-user", upload_key), limit):
                return await reject(429, "Too many uploads")
            if upload_key in self.uploading or len(self.uploading) >= 4:
                return await reject(429, "Upload already in progress")
            self.uploading.add(upload_key)
        try:
            await self.app(scope, receive, secured_send)
        finally:
            if upload_key is not None:
                self.uploading.discard(upload_key)
