import logging
import os

from fastapi import FastAPI, Response, WebSocket
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, ValidationError

from .broker import mqtt
from .websocket import ws_manager

logger = logging.getLogger(__name__)

app = FastAPI()
mqtt.init_app(app)

background_task_started = False

app.mount("/static", StaticFiles(directory="static", html=True), name="static")


@app.get("/")
async def main():
    with open(os.path.join("static", "index.html")) as fh:
        data = fh.read()
    return Response(content=data, media_type="text/html")


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
                logger.error("Cannot parse %s %s", cmd, err)
                continue
            logger.debug("putting %s in command queue", req)
            mqtt.client.publish("/blind/cmd", f"{req.blind} {req.position}")
            ws_manager.command_q.put_nowait(req)

    await receive_command(websocket)


def start():
    import uvicorn

    logging.basicConfig(level=logging.DEBUG)
    uvicorn.run(app, host="0.0.0.0", port=8000)


if __name__ == "__main__":
    start()
