import logging
import os
import pickle
from secrets import token_urlsafe
from typing import Optional

from authlib.integrations.starlette_client import OAuth, OAuthError
from fastapi import Cookie, FastAPI, WebSocket
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, ValidationError
from starlette.config import Config
from starlette.middleware.sessions import SessionMiddleware
from starlette.requests import Request
from starlette.responses import HTMLResponse, RedirectResponse

from .autoryzowane import authorized
from .broker import mqtt
from .websocket import ws_manager

logger = logging.getLogger(__name__)

app = FastAPI()
app.add_middleware(SessionMiddleware, secret_key="!secret")
mqtt.init_app(app)

config = Config("turbacz/.env")
oauth = OAuth(config)

background_task_started = False

app.mount("/static", StaticFiles(directory="static", html=True), name="static")

CONF_URL = "https://accounts.google.com/.well-known/openid-configuration"
oauth.register(
    name="google",
    server_metadata_url=CONF_URL,
    client_kwargs={"scope": "openid email profile"},
)

with open('turbacz/cookies.pickle', 'rb') as cookies:
    access_cookies = pickle.load(cookies)


@app.get("/")
async def homepage(request: Request, access_token: Optional[str] = Cookie(None)):
    user = request.session.get("user")
    if access_token and access_token in access_cookies:
        return RedirectResponse(url="/auto")
    if user:
        return HTMLResponse('<p>invalid email!</p><a href="/login">login</a>')
    return HTMLResponse('<a href="/login">login</a>')


@app.get("/auto")
async def main(request: Request):
    user = request.session.get("user")
    if user:
        with open(os.path.join("static", "index.html")) as fh:
            data = fh.read()
        return HTMLResponse(content=data, media_type="text/html")
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
            access_token = token_urlsafe()
            access_cookies[access_token] = user["email"]
            with open("turbacz/cookies.pickle", "wb") as cookies:
                pickle.dump(access_cookies, cookies)
            response = RedirectResponse(url="/auto")
            response.set_cookie("access_token", access_token, max_age=3600*12)
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


@app.websocket("/ws/{client_id}")
async def websocket_endpoint(websocket: WebSocket, access_token=Cookie()):
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

    if access_token in access_cookies:
        await receive_command(websocket)


def start():
    import uvicorn

    logging.basicConfig(level=logging.DEBUG)
    uvicorn.run(app, host="0.0.0.0", port=8000)


if __name__ == "__main__":
    start()
