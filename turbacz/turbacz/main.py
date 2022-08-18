import asyncio
import json
import os
from typing import Any, List

from fastapi import FastAPI, Response, WebSocket
from fastapi.staticfiles import StaticFiles
from fastapi_mqtt import FastMQTT, MQTTConfig
from pydantic import BaseModel, ValidationError

app = FastAPI()

mqtt_config = MQTTConfig(
    host="127.0.0.1",
    port=1883,
    keepalive=60,
    username="aplikacja_webowa",
    password="password",
)

mqtt = FastMQTT(config=mqtt_config)

mqtt.init_app(app)


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

    async def broadcast(self, message: Any):
        for connection in self.active_connections:
            await connection.send_json(message)


ws_manager = ConnectionManager()

background_task_started = False

app.mount("/static", StaticFiles(directory="static", html=True), name="static")


@app.get("/")
async def main():
    with open(os.path.join("static", "suwanie_rolet2.html")) as fh:
        data = fh.read()
    return Response(content=data, media_type="text/html")


@mqtt.on_connect()
def connect(client, flags, rc, properties):
    mqtt.client.subscribe("/blind/pos")  # subscribing mqtt topic
    mqtt.publish("/blind/cmd", "S")
    print("Connected: ", client, flags, rc, properties)


@mqtt.on_message()
async def message(client, topic, payload, qos, properties):
    payload = json.loads(payload.decode())
    print("Received message: ", topic, payload, qos, properties)
    print(type(payload), payload)
    for blind, pos in payload.items():
        await ws_manager.broadcast({"blind": blind, "current_position": pos})


class BlindRequest(BaseModel):
    blind: str
    position: int


@app.post("/setblind")
async def set_blind(req: BlindRequest):
    return {"current_position": req.position}


@app.websocket("/ws/{client_id}")
async def websocket_endpoint(websocket: WebSocket, client_id: int):
    await ws_manager.connect(websocket)
    mqtt.publish("/blind/cmd", "S")

    async def receive_command(websocket: WebSocket):
        async for cmd in websocket.iter_json():
            try:
                req = BlindRequest.parse_obj(cmd)
            except ValidationError as err:
                print("Cannot parse", cmd, err)
                continue
            print(f"putting {req} in command queue")
            mqtt.client.publish("/blind/cmd", f"{req.blind} {req.position}")
            ws_manager.command_q.put_nowait(req)

    await receive_command(websocket)


def start():
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)


if __name__ == "__main__":
    start()
