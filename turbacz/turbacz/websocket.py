import asyncio
import logging
from typing import Any, List

from fastapi import WebSocket
from starlette.websockets import WebSocketDisconnect
from uvicorn.protocols.utils import ClientDisconnected

logger = logging.getLogger(__name__)


class ConnectionManager:
    def __init__(self):
        self.active_connections: List[WebSocket] = []
        self.command_q = asyncio.queues.Queue()

    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        self.active_connections.append(websocket)

    def disconnect(self, connection):
        if connection in self.active_connections:
            self.active_connections.remove(connection)

    async def send_personal_message(self, message: Any, websocket: WebSocket):
        try:
            await websocket.send_json(message)
        except (WebSocketDisconnect, RuntimeError, ConnectionResetError) as err:
            logger.warning(
                "Error sending personal message to %s: %s", websocket, err.args[0]
            )

    async def broadcast(self, message: Any, app: Any):
        for connection in tuple(self.active_connections):
            if app in connection.url.path:
                try:
                    await connection.send_json(message)
                except (
                    WebSocketDisconnect,
                    ClientDisconnected,
                    RuntimeError,
                    ConnectionResetError,
                ) as err:
                    self.disconnect(connection)
                    logger.warning(
                        "removing closed connection %s (%s)", connection, str(err)
                    )


ws_manager = ConnectionManager()
