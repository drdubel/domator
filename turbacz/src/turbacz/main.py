import logging
import os
from pickle import dump, load
from secrets import token_urlsafe
from typing import Optional
import asyncio

import httpx
from aioprometheus.asgi.middleware import MetricsMiddleware
from aioprometheus.asgi.starlette import metrics
from authlib.integrations.starlette_client import OAuth, OAuthError
from fastapi import Cookie, FastAPI, Response, WebSocket
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, ValidationError
from starlette.exceptions import HTTPException as StarletteHTTPException
from starlette.middleware.sessions import SessionMiddleware
from starlette.requests import Request
from starlette.responses import HTMLResponse, RedirectResponse

from .broker import mqtt
from .config import settings
from .websocket import ws_manager

logger = logging.getLogger(__name__)

app = FastAPI()
app.add_middleware(SessionMiddleware, secret_key=settings.session_secret)
app.add_middleware(MetricsMiddleware)
app.add_route("/metrics", metrics)
mqtt.init_app(app)

oauth = OAuth()
oauth.register(
    name="google",
    client_id=settings.oauth.client_id,
    client_secret=settings.oauth.client_secret,
    server_metadata_url=settings.oauth.configuration_url,
    client_kwargs={"scope": "openid email profile"},
)

background_task_started = False

app.mount("/static", StaticFiles(directory="./static", html=True), name="static")


def save_cookies(cookies):
    with open(settings.cookies_path, "wb") as cookies:
        dump(access_cookies, cookies)

def load_cookies():
    with open(settings.cookies_path, "rb") as cookies:
        return load(cookies)


access_cookies = load_cookies()


@app.exception_handler(StarletteHTTPException)
async def custom_http_exception_handler(request: Request, exc):
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


async def vm_query(query):
    async with httpx.AsyncClient() as client:
        resp = await client.get(
            f"{settings.victoria_metrics_url}/api/v1/query?query={query}"
        )
        assert resp.status_code == 200
    return resp.json()["data"]["result"]


async def vm_query_range(query, start, end, step):
    async with httpx.AsyncClient() as client:
        resp = await client.get(
            f"{settings.victoria_metrics_url}/api/v1/query_range",
            params={
                "start": start,
                "end": end,
                "query": query,
                "step": step,
            },
        )
        assert resp.status_code == 200
    return resp.json()["data"]["result"]


@app.get("/api/temperatures")
async def get_temperatures(start: int, end: int, step: int):
    water_temperatures = sum(await asyncio.gather(
        vm_query_range("water_temperatures", start, end, step),
        vm_query_range("pid_target", start, end, step),
    ), start=[])
    return [
        {
            "timestamp": water_temperatures[0]["values"][i][0],
            "cold": water_temperatures[0]["values"][i][1],
            "hot": water_temperatures[1]["values"][i][1],
            "mixed": water_temperatures[2]["values"][i][1],
            "target": water_temperatures[3]["values"][i][1],
        }
        for i in range(len(water_temperatures[0]["values"]))
    ]


@app.get("/api/heating_data")
async def get_heating_data():
    heating_data = sum(await asyncio.gather(
        vm_query("pid_target"),
        vm_query("pid_integral"),
        vm_query("pid_ouput"),
        vm_query("pid_multiplier"),
    ), start=[])
    return {
        "cold": heating_data[0]["value"][1],
        "hot": heating_data[1]["value"][1],
        "mixed": heating_data[2]["value"][1],
        "target": heating_data[3]["value"][1],
        "integral": heating_data[4]["value"][1],
        "pid": heating_data[5]["value"][1],
        "kd": heating_data[6]["value"][1],
        "ki": heating_data[7]["value"][1],
        "kp": heating_data[8]["value"][1],
    }


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
        if user["email"] in settings.authorized_users:
            access_token = token_urlsafe()
            access_cookies[access_token] = user["email"]
            save_cookies(access_cookies)
            response = RedirectResponse(url="/auto")
            response.set_cookie("access_token", access_token, max_age=3600 * 24 * 14)
            return response
        else:
            return RedirectResponse(url="/")


@app.get("/logout")
async def logout(
    request: Request, response: Response, access_token: Optional[str] = Cookie(None)
):
    access_cookies.pop(access_token)
    save_cookies(access_cookies)
    request.session.pop("user", None)
    response.delete_cookie(key="access_token")
    return RedirectResponse(url="/")


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


@app.websocket("/lights/ws/{client_id}")
async def websocket_lights(websocket: WebSocket, access_token=Cookie()):
    await ws_manager.connect(websocket)
    mqtt.publish("/switch/1/cmd", "S")

    async def receive_command(websocket: WebSocket):
        async for cmd in websocket.iter_json():
            try:
                chg = SwitchChange.parse_obj(cmd)
            except ValidationError as err:
                logger.error("Cannot parse %s %s", cmd, err)
                continue
            logger.debug("putting %s in command queue", cmd)
            print(f"{chg.id}{chg.state}")
            mqtt.client.publish("/switch/1/cmd", f"{chg.id}{chg.state}")

    if access_token in access_cookies:
        await receive_command(websocket)


def start():
    import uvicorn

    logging.basicConfig(level=logging.DEBUG)
    uvicorn.run(app, host="127.0.0.1", port=8000)


if __name__ == "__main__":
    start()
