import os
from datetime import datetime, timedelta, timezone
from typing import Optional

from authlib.integrations.starlette_client import OAuth, OAuthError
from fastapi import APIRouter, Cookie, Request, Response, WebSocket
from jose import JWTError, jwt
from starlette.responses import HTMLResponse, RedirectResponse

from turbacz.settings import config

router = APIRouter()

JWT_ALG = "HS256"
JWT_EXPIRE_MINUTES = 60 * 24 * 14  # 14 days

oauth = OAuth()
oauth.register(
    config.oidc.provider,
    client_id=config.oidc.client_id,
    client_secret=config.oidc.client_secret,
    server_metadata_url=config.oidc.server_metadata_url,
    client_kwargs={"scope": "openid email profile"},
)


def create_jwt(data: dict) -> str:
    now = datetime.now(timezone.utc)
    payload = data.copy()
    payload.update(
        {
            "iat": now,
            "exp": now + timedelta(minutes=JWT_EXPIRE_MINUTES),
        }
    )

    return jwt.encode(payload, config.jwt_secret, algorithm=JWT_ALG)


def verify_jwt(token: str) -> dict | None:
    try:
        return jwt.decode(token, config.jwt_secret, algorithms=[JWT_ALG])

    except JWTError:
        return None


def get_current_user(access_token: str | None) -> dict | None:
    if not access_token:
        return None

    return verify_jwt(access_token)


async def websocket_auth(websocket: WebSocket) -> dict | None:
    token = websocket.query_params.get("token")

    if not token:
        return None

    user = verify_jwt(token)
    if not user:
        return None

    return user


@router.get("/login")
async def login(request: Request):
    redirect_uri = request.url_for("auth")
    return await oauth.google.authorize_redirect(request, redirect_uri)


@router.get("/auth")
async def auth(request: Request):
    try:
        token = await oauth.google.authorize_access_token(request)

    except OAuthError as error:
        return HTMLResponse(f"<h1>{error.error}</h1>")

    user = token.get("userinfo")

    if user and user["email"] in config.authorized:
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


@router.get("/logout")
async def logout(response: Response):
    response = RedirectResponse(url="/")
    response.delete_cookie("access_token")

    return response


@router.get("/auto")
async def main(request: Request, access_token: Optional[str] = Cookie(None)):
    user = get_current_user(access_token)

    if user:
        with open(os.path.join("static", "index.html")) as fh:
            data = fh.read()

        return Response(content=data, media_type="text/html")

    return RedirectResponse(url="/")
