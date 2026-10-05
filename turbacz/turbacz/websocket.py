import asyncio
import json
import logging
import math
from functools import wraps

from fastapi import HTTPException, WebSocket
from starlette.websockets import WebSocketDisconnect
from turbacz.database import db_call
from turbacz.security import RateLimiter
from turbacz.settings import config
from turbacz.validation import validate_command

logger = logging.getLogger(__name__)


def bounded_json(value, remaining, depth=0):
    if depth > 16:
        raise ValueError("Message nesting limit exceeded")
    remaining[0] -= 1
    if remaining[0] < 0:
        raise ValueError("Message collection limit exceeded")
    if isinstance(value, dict):
        for item in value.values():
            bounded_json(item, remaining, depth + 1)
    elif isinstance(value, list):
        for item in value:
            bounded_json(item, remaining, depth + 1)
    elif isinstance(value, float) and not math.isfinite(value):
        raise ValueError("Non-finite number")


class ConnectionManager:
    def __init__(self):
        self.active_connections = []
        self.clients = {}
        self.send_locks = {}
        self.limiter = RateLimiter()

    async def connect(self, websocket, principal):
        limits = config.security
        ip = websocket.client.host if websocket.client else "unknown"
        clients = list(self.clients.values())
        if (
            len(clients) >= limits.ws_connections_total
            or sum(user == principal for user, _ in clients)
            >= limits.ws_connections_per_user
            or sum(peer == ip for _, peer in clients) >= limits.ws_connections_per_ip
        ):
            await websocket.close(code=1013)
            return False
        # Reserve before awaiting accept so simultaneous handshakes obey limits.
        self.clients[websocket] = (principal, ip)
        self.send_locks[websocket] = asyncio.Lock()
        try:
            await websocket.accept()
            self.active_connections.append(websocket)
        except BaseException:
            self.disconnect(websocket)
            raise
        return True

    def disconnect(self, connection):
        if connection in self.active_connections:
            self.active_connections.remove(connection)
        self.clients.pop(connection, None)
        self.send_locks.pop(connection, None)

    async def close(self, websocket, code):
        self.disconnect(websocket)
        try:
            await asyncio.wait_for(
                websocket.close(code=code), config.security.ws_send_timeout
            )
        except (TimeoutError, RuntimeError, OSError, WebSocketDisconnect):
            pass

    def endpoint(self, handler):
        @wraps(handler)
        async def run(websocket: WebSocket):
            from turbacz.auth import websocket_auth

            user = await websocket_auth(websocket)
            if not user or not isinstance(user.get("sub"), str):
                await websocket.close(code=1008)
                return
            if not await self.connect(websocket, user["sub"]):
                return
            try:
                await handler(websocket)
            except (WebSocketDisconnect, OSError):
                pass
            finally:
                await self.close(websocket, 1000)

        return run

    async def messages(self, websocket):
        from turbacz.connection_manager import connection_manager

        while websocket in self.clients:
            try:
                message = await asyncio.wait_for(
                    websocket.receive(), config.security.ws_idle_seconds
                )
            except TimeoutError:
                await self.close(websocket, 1001)
                return
            except RuntimeError:
                if websocket not in self.clients:
                    return
                raise
            if message["type"] == "websocket.disconnect":
                return
            raw = message.get("text")
            if raw is None:
                await self.close(websocket, 1003)
                return
            if len(raw.encode("utf-8")) > config.security.ws_max_bytes:
                await self.close(websocket, 1009)
                return
            identity = self.clients.get(websocket)
            if identity is None:
                return
            principal, ip = identity
            limit = config.security.ws_messages_per_minute
            if not self.limiter.allow(
                ("user", principal), limit
            ) or not self.limiter.allow(("ip", ip), limit * 2):
                await self.close(websocket, 1013)
                return
            try:
                cmd = json.loads(raw)
                bounded_json(cmd, [config.security.ws_max_items])
                if cmd == {"type": "ping"}:
                    await self.send_personal_message({"type": "pong"}, websocket)
                    continue
                await db_call(
                    validate_command, websocket.url.path, cmd, connection_manager
                )
            except (ValueError, TypeError, KeyError, RecursionError, HTTPException):
                await self.close(websocket, 1008)
                return
            yield cmd

    async def send_personal_message(self, message, websocket):
        lock = self.send_locks.get(websocket)
        if lock is None:
            return
        try:
            # Bound both lock wait and slow transport writes.
            async with asyncio.timeout(config.security.ws_send_timeout):
                async with lock:
                    await websocket.send_json(message)
        except (TimeoutError, WebSocketDisconnect, RuntimeError, OSError):
            await self.close(websocket, 1013)

    async def broadcast(self, message, app):
        await asyncio.gather(
            *(
                self.send_personal_message(message, connection)
                for connection in tuple(self.active_connections)
                if app in connection.url.path
            )
        )


ws_manager = ConnectionManager()
