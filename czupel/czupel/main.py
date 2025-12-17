import json
import logging
import os
from pathlib import Path
from typing import Optional

import sentry_sdk
from aioprometheus.asgi.middleware import MetricsMiddleware
from aioprometheus.asgi.starlette import metrics
from authlib.integrations.starlette_client import OAuth, OAuthError
from fastapi import Cookie, FastAPI, File, Response, UploadFile, WebSocket
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, ValidationError
from starlette.config import Config
from starlette.exceptions import HTTPException as StarletteHTTPException
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.middleware.sessions import SessionMiddleware
from starlette.requests import Request
from starlette.responses import HTMLResponse, RedirectResponse
from starlette.types import ASGIApp

from czupel.auth import JWT_EXPIRE_MINUTES, create_jwt, get_current_user, websocket_auth
from czupel.broker import mqtt
from czupel.data.authorized import authorized
from czupel.state import relay_state
from czupel.websocket import ws_manager
from czupel.data.secrets import DSN

logger = logging.getLogger(__name__)


class CustomRequestSizeMiddleware(BaseHTTPMiddleware):
    def __init__(self, app: ASGIApp, max_content_size: int):
        super().__init__(app)
        self.max_content_size = max_content_size

    async def dispatch(self, request: Request, call_next: callable) -> Response:
        content_length = int(request.headers.get("content-length", 0))
        if content_length > self.max_content_size:
            return Response(
                content="Request body too large",
                status_code=413,
                media_type="text/plain",
            )
        return await call_next(request)


MAX_REQUEST_SIZE = 10_000_000

sentry_sdk.init(
    dsn=DSN,
    send_default_pii=True,
    traces_sample_rate=1.0,
)

app = FastAPI()
app.add_middleware(CustomRequestSizeMiddleware, max_content_size=MAX_REQUEST_SIZE)
app.add_middleware(SessionMiddleware, secret_key="!secret")
app.add_middleware(MetricsMiddleware)
app.add_route("/metrics", metrics)
mqtt.init_app(app)

config = Config("czupel/data/.env")
oauth = OAuth(config)

background_task_started = False

app.mount("/static", StaticFiles(directory="./static", html=True), name="static")


CONF_URL = "https://accounts.google.com/.well-known/openid-configuration"
oauth.register(
    name="google",
    server_metadata_url=CONF_URL,
    client_kwargs={"scope": "openid email profile"},
)


@app.exception_handler(StarletteHTTPException)
async def custom_http_exception_handler(request: Request, exc):
    return HTMLResponse(
        '<h1>Sio!<br>Tu nic nie ma!</h1><a href="/auto">Strona Główna</a>'
    )


@app.get("/")
async def homepage(request: Request, access_token: Optional[str] = Cookie(None)):
    user = get_current_user(access_token)

    if user:
        return RedirectResponse(url="/auto")

    return HTMLResponse('<a href="/login">login</a>')


@app.get("/login")
async def login(request: Request):
    redirect_uri = request.url_for("auth")
    return await oauth.google.authorize_redirect(request, redirect_uri)


@app.get("/auth")
async def auth(request: Request):
    try:
        token = await oauth.google.authorize_access_token(request)

    except OAuthError as error:
        return HTMLResponse(f"<h1>{error.error}</h1>")

    user = token.get("userinfo")

    if user and user["email"] in authorized:
        jwt_token = create_jwt(
            {
                "sub": user["email"],
                "name": user.get("name"),
            }
        )

        response = RedirectResponse(url="/auto")
        response.set_cookie(
            "access_token",
            jwt_token,
            httponly=False,
            secure=True,  # Set to False in development if not using HTTPS
            samesite="lax",
            max_age=JWT_EXPIRE_MINUTES * 60,
        )

        return response

    return RedirectResponse(url="/")


@app.get("/logout")
async def logout(response: Response):
    response = RedirectResponse(url="/")
    response.delete_cookie("access_token")

    return response


@app.get("/auto")
async def main(request: Request, access_token: Optional[str] = Cookie(None)):
    user = get_current_user(access_token)

    if user:
        with open(os.path.join("static", "index.html")) as fh:
            data = fh.read()

        return Response(content=data, media_type="text/html")

    return RedirectResponse(url="/")


@app.get("/heating")
async def heating(request: Request, access_token: Optional[str] = Cookie(None)):
    user = get_current_user(access_token)

    if user:
        with open(os.path.join("static", "heating.html")) as fh:
            data = fh.read()

        return Response(content=data, media_type="text/html")

    return RedirectResponse(url="/")


@app.get("/blinds")
async def blinds(request: Request, access_token: Optional[str] = Cookie(None)):
    user = get_current_user(access_token)

    if user:
        with open(os.path.join("static", "blinds.html")) as fh:
            data = fh.read()

        return Response(content=data, media_type="text/html")

    return RedirectResponse(url="/")


@app.get("/lights")
async def lights(request: Request, access_token: Optional[str] = Cookie(None)):
    user = get_current_user(access_token)

    if user:
        with open(os.path.join("static", "lights.html")) as fh:
            data = fh.read()

        return Response(content=data, media_type="text/html")

    return RedirectResponse(url="/")


@app.get("/rcm")
async def rcm_page(request: Request, access_token: Optional[str] = Cookie(None)):
    user = get_current_user(access_token)

    if not user:
        return RedirectResponse(url="/")

    with open(os.path.join("static", "relay_connection_manager.html")) as fh:
        data = fh.read()

    return Response(content=data, media_type="text/html")


@app.get("/upload")
async def upload_page(request: Request, access_token: Optional[str] = Cookie(None)):
    user = get_current_user(access_token)

    if not user:
        return RedirectResponse(url="/")

    with open(os.path.join("static", "upload.html")) as fh:
        data = fh.read()

    return Response(content=data, media_type="text/html")


@app.post("/upload/{device}")
async def upload_firmware(
    request: Request,
    device: str,
    file: UploadFile = File(...),
    access_token: Optional[str] = Cookie(None),
):
    user = get_current_user(access_token)

    if not user:
        return RedirectResponse(url="/")

    print("Uploading firmware for device:", device)

    if device not in ["switch", "relay", "root"]:
        return JSONResponse(
            {"status": "error", "reason": "unknown device"}, status_code=400
        )

    try:
        save_path = Path(f"static/data/{device}/firmware.bin")
        save_path.parent.mkdir(parents=True, exist_ok=True)

        contents = await file.read()

        with open(save_path, "wb") as buffer:
            buffer.write(contents)

        logger.info(f"Successfully saved {len(contents)} bytes to {save_path}")
        return JSONResponse({"status": "ok", "device": device, "size": len(contents)})

    except Exception as e:
        logger.error(f"Error uploading firmware: {e}", exc_info=True)
        return JSONResponse({"status": "error", "reason": str(e)}, status_code=500)


class BlindRequest(BaseModel):
    blind: str
    position: int


class SwitchChange(BaseModel):
    id: str
    state: int


@app.post("/setblind")
async def set_blind(req: BlindRequest):
    return {"current_position": req.position}


@app.websocket("/blinds/ws/{client_id}")
async def websocket_blinds(websocket: WebSocket):
    user = await websocket_auth(websocket)

    if not user:
        await websocket.close(code=1008)

        return

    await ws_manager.connect(websocket)

    mqtt.publish("/blind/cmd", "S")

    async def receive_command(websocket: WebSocket):
        async for cmd in websocket.iter_json():
            try:
                req = BlindRequest.model_validate(cmd)
            except ValidationError as err:
                logger.error("Cannot parse %s %s", cmd, err)
                continue
            logger.debug("putting %s in command queue", req)
            mqtt.client.publish(
                "/blind/cmd", f"{chr(int(req.blind[1])+96)}{req.position}"
            )

    await receive_command(websocket)


@app.websocket("/heating/ws/{client_id}")
async def websocket_heating(websocket: WebSocket):
    user = await websocket_auth(websocket)

    if not user:
        await websocket.close(code=1008)

        return

    await ws_manager.connect(websocket)

    async def receive_command(websocket: WebSocket):
        async for cmd in websocket.iter_json():
            logger.debug("putting %s in command queue", cmd)
            mqtt.client.publish("/heating/cmd", cmd)

    await receive_command(websocket)


@app.websocket("/lights/ws/{client_id}")
async def websocket_lights(websocket: WebSocket):
    user = await websocket_auth(websocket)

    if not user:
        await websocket.close(code=1008)

        return

    await ws_manager.connect(websocket)

    current_states = relay_state.get_all()
    for light_id, state in current_states.items():
        await ws_manager.send_personal_message(
            {"id": light_id, "state": state}, websocket
        )

    async def receive_command(websocket: WebSocket):
        async for cmd in websocket.iter_json():
            try:
                chg = SwitchChange.model_validate(cmd)
            except ValidationError as err:
                logger.error("Cannot parse %s %s", cmd, err)
                continue
            logger.debug("putting %s in command queue", cmd)
            chg.id = int(chg.id[1:])

            match chg.id // 8:
                case 0:
                    topic = "/switch/1/cmd"

                case 1:
                    topic = "/relay/cmd/1074130365"

                case 2:
                    topic = "/relay/cmd/1074122133"

                case _:
                    raise ValueError("Invalid light id")

            chg.id = chr(chg.id % 8 + 97)

            print(topic, f"{chg.id}{chg.state}")  # Debug print
            mqtt.client.publish(topic, f"{chg.id}{chg.state}")

    await receive_command(websocket)


@app.websocket("/rcm/ws/{client_id}")
async def websocket_rcm(websocket: WebSocket):
    user = await websocket_auth(websocket)

    if not user:
        await websocket.close(code=1008)

        return

    await ws_manager.connect(websocket)

    with open("czupel/data/connections.json", "r", encoding="utf-8") as f:
        rcm_config = json.load(f)
        print(rcm_config)

        await ws_manager.send_personal_message(rcm_config, websocket)

    async def receive_command(websocket: WebSocket):
        async for cmd in websocket.iter_json():
            logger.debug("putting %s in command queue", cmd)

            with open("czupel/data/connections.json", "w", encoding="utf-8") as f:
                json.dump(cmd, f, ensure_ascii=False, indent=2)

            for connection in list(cmd["connections"].keys()):
                cmd["connections"][connection[:10]] = cmd["connections"].pop(connection)

            print(f"Updated connections: {cmd['connections']}")

            mqtt.client.publish("/switch/cmd/root", cmd["connections"])

    await receive_command(websocket)


def start():
    import uvicorn

    logging.basicConfig(level=logging.DEBUG)
    uvicorn.run("czupel.main:app", port=8002, reload=True)


if __name__ == "__main__":
    start()
