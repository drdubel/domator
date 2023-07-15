import logging
import os
from pickle import dump, load
from secrets import token_urlsafe
from typing import Optional

from aioprometheus.asgi.middleware import MetricsMiddleware
from aioprometheus.asgi.starlette import metrics
from authlib.integrations.starlette_client import OAuth, OAuthError
from fastapi import Cookie, FastAPI, Response, WebSocket
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, ValidationError
from starlette.config import Config
from starlette.exceptions import HTTPException as StarletteHTTPException
from starlette.middleware.sessions import SessionMiddleware
from starlette.requests import Request
from starlette.responses import HTMLResponse, RedirectResponse

from .broker import mqtt
from .data.authorized import authorized
from .websocket import ws_manager

logger = logging.getLogger(__name__)

app = FastAPI()
app.add_middleware(SessionMiddleware, secret_key="!secret")
app.add_middleware(MetricsMiddleware)
app.add_route("/metrics", metrics)
mqtt.init_app(app)

config = Config("turbacz/data/.env")
oauth = OAuth(config)

background_task_started = False

app.mount("/static", StaticFiles(directory="./static", html=True), name="static")

CONF_URL = "https://accounts.google.com/.well-known/openid-configuration"
oauth.register(
    name="google",
    server_metadata_url=CONF_URL,
    client_kwargs={"scope": "openid email profile"},
)

with open("turbacz/data/cookies.pickle", "rb") as cookies:
    access_cookies = load(cookies)


@app.exception_handler(StarletteHTTPException)
async def custom_http_exception_handler(request, exc):
    return HTMLResponse(
        '<h1>Sio!<br>Tu nic nie ma!</h1><a href="/auto">Strona Główna</a>'
    )


@app.get("/")
async def homepage(request: Request, access_token: Optional[str] = Cookie(None)):
    user = request.session.get("user")
    if access_token in access_cookies:
        return RedirectResponse(url="/auto")
    if user:
        return HTMLResponse('<h1>Sio!</h1><a href="/login">login</a>')
    return HTMLResponse('<a href="/login">login</a>')


@app.get("/auto")
async def main(request: Request, access_token: Optional[str] = Cookie(None)):
    user = request.session.get("user")
    if user and access_token in access_cookies:
        with open(os.path.join("static", "index.html")) as fh:
            data = fh.read()
        return Response(content=data, media_type="text/html")
    return RedirectResponse(url="/")


@app.get("/heating")
async def heating(request: Request, access_token: Optional[str] = Cookie(None)):
    user = request.session.get("user")
    if user and access_token in access_cookies:
        with open(os.path.join("static", "heating.html")) as fh:
            data = fh.read()
        return Response(content=data, media_type="text/html")
    return RedirectResponse(url="/")


@app.get("/blinds")
async def blinds(request: Request, access_token: Optional[str] = Cookie(None)):
    user = request.session.get("user")
    if user and access_token in access_cookies:
        with open(os.path.join("static", "blinds.html")) as fh:
            data = fh.read()
        return Response(content=data, media_type="text/html")
    return RedirectResponse(url="/")


@app.get("/lights")
async def lights(request: Request, access_token: Optional[str] = Cookie(None)):
    user = request.session.get("user")
    if user and access_token in access_cookies:
        with open(os.path.join("static", "lights.html")) as fh:
            data = fh.read()
        return Response(content=data, media_type="text/html")
    return RedirectResponse(url="/")


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
    if user:
        request.session["user"] = dict(user)
        if user["email"] in authorized:
            print(user)
            access_token = token_urlsafe()
            access_cookies[access_token] = user["email"]
            with open("turbacz/data/cookies.pickle", "wb") as cookies:
                dump(access_cookies, cookies)
            response = RedirectResponse(url="/auto")
            response.set_cookie("access_token", access_token, max_age=3600 * 24 * 14)
            return response
        else:
            return RedirectResponse(url="/")


@app.get("/logout")
async def logout(request: Request):
    request.session.pop("user", None)
    return RedirectResponse(url="/")


class BlindRequest(BaseModel):
    blind: str
    position: int


@app.post("/setblind")
async def set_blind(req: BlindRequest):
    return {"current_position": req.position}


@app.websocket("/blinds/ws/{client_id}")
async def websocket_blinds(websocket: WebSocket, access_token=Cookie()):
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
            mqtt.client.publish(
                "/blind/cmd", f"{chr(int(req.blind[1])+96)}{req.position}"
            )

    if access_token in access_cookies:
        await receive_command(websocket)


@app.websocket("/heating/ws/{client_id}")
async def websocket_heating(websocket: WebSocket, access_token=Cookie()):
    await ws_manager.connect(websocket)

    async def receive_command(websocket: WebSocket):
        async for cmd in websocket.iter_json():
            logger.debug("putting %s in command queue", cmd)
            mqtt.client.publish("/heating/cmd", cmd)

    if access_token in access_cookies:
        await receive_command(websocket)


def start():
    import uvicorn

    logging.basicConfig(level=logging.DEBUG)
    uvicorn.run(app, host="127.0.0.1", port=8000)


if __name__ == "__main__":
    start()
