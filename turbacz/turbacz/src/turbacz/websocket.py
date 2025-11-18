import asyncio
from csv import Error
import logging
from typing import Any, List

from fastapi import WebSocket, WebSocketDisconnect
from wsproto.utilities import LocalProtocolError

logger = logging.getLogger(__name__)


class ConnectionManager:
    def __init__(self):
        self.active_connections: List[WebSocket] = []
        self.command_q = asyncio.queues.Queue()

    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        self.active_connections.append(websocket)

    def disconnect(self, websocket: WebSocket):
        self.active_connections.remove(websocket)

    async def send_personal_message(self, message: Any, websocket: WebSocket):
        await websocket.send_json(message)

    async def broadcast(self, message: Any, app: Any):
        for connection in tuple(self.active_connections):
            try:
                if app in connection.url.path:
                    await connection.send_json(message)
            except WebSocketDisconnect as err:
                self.active_connections.remove(connection)
                logger.warning(
                    "removing closed connection %s (%s)", connection, err.args[0]
                )
                continue


ws_manager = ConnectionManager()
