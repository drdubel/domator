import html
import os
from datetime import datetime, timedelta, timezone
from typing import Optional

import jwt
from authlib.integrations.starlette_client import OAuth, OAuthError
from fastapi import APIRouter, Cookie, HTTPException, Request, Response, WebSocket
from jwt import InvalidTokenError
from starlette.responses import HTMLResponse, RedirectResponse
from turbacz.settings import config

router = APIRouter()

JWT_ALG = "HS256"
JWT_EXPIRE_MINUTES = 60 * 24 * 14  # 14 days
JWT_SECRET = config.jwt_secret
if config.oidc.allow_insecure_http:
    os.environ["AUTHLIB_INSECURE_TRANSPORT"] = "1"
if not JWT_SECRET:
    raise RuntimeError("Missing jwt_secret in turbacz.toml. Set jwt_secret to a stable value before starting the app.")

oauth = OAuth()
register_kwargs = {
    "client_id": config.oidc.client_id,
    "client_secret": config.oidc.client_secret,
    "server_metadata_url": config.oidc.server_metadata_url,
    "client_kwargs": {"scope": "openid email profile"},
}
if config.oidc.token_endpoint_auth_method:
    register_kwargs["token_endpoint_auth_method"] = config.oidc.token_endpoint_auth_method
oauth.register(config.oidc.provider, **register_kwargs)


def create_jwt(data: dict) -> str:
    now = datetime.now(timezone.utc)
    payload = data.copy()
    payload.update(
        {
            "iat": now,
            "exp": now + timedelta(minutes=JWT_EXPIRE_MINUTES),
        }
    )

    return jwt.encode(payload, JWT_SECRET, algorithm=JWT_ALG)


def verify_jwt(token: str) -> dict | None:
    try:
        return jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALG], options={"require": ["exp", "iat", "sub"]})

    except InvalidTokenError:
        return None


def get_current_user(access_token: str | None) -> dict | None:
    if not access_token:
        return None

    return verify_jwt(access_token)


def bearer_token_from_header(authorization: str | None) -> str | None:
    if not authorization or not authorization.startswith("Bearer "):
        return None

    return authorization.removeprefix("Bearer ")


async def websocket_auth(websocket: WebSocket) -> dict | None:
    from turbacz.security import trusted_host, trusted_origin

    if not trusted_host(websocket):
        return None
    origin = websocket.headers.get("origin")
    if origin is not None and not trusted_origin(websocket):
        return None
    explicit = bearer_token_from_header(websocket.headers.get("authorization")) or websocket.query_params.get("token")
    # A browser cookie alone is never accepted without a trusted Origin.
    if not origin and not explicit:
        return None
    token = explicit or websocket.cookies.get("access_token")

    if not token:
        return None

    user = verify_jwt(token)
    if not user:
        return None

    return user


@router.get("/login")
async def login(request: Request, client: Optional[str] = None):
    request.session["mobile_login"] = client == "mobile"

    redirect_uri = config.oidc.redirect_uri or str(request.url_for("auth"))
    return await oauth.google.authorize_redirect(request, redirect_uri)


@router.get("/auth")
async def auth(request: Request):
    def _format_oauth_error(error: OAuthError) -> str:
        # error/description come straight from the query string and are attacker
        # controlled, so they must be escaped before being put in the response.
        code = html.escape(str(error.error or "unknown_error"))

        if not error.description:
            return code

        return f"{code}: {html.escape(str(error.description))}"

    def _is_invalid_client(error: OAuthError) -> bool:
        return error.error == "invalid_client"

    try:
        token = await oauth.google.authorize_access_token(request)

    except OAuthError as error:
        if not config.oidc.token_endpoint_auth_method and _is_invalid_client(error):
            for method in ("client_secret_post", "client_secret_basic"):
                try:
                    token = await oauth.google.authorize_access_token(request, token_endpoint_auth_method=method)
                    break
                except OAuthError:
                    continue
            else:
                return HTMLResponse(f"<h1>{_format_oauth_error(error)}</h1>")
        else:
            return HTMLResponse(f"<h1>{_format_oauth_error(error)}</h1>")

    user = token.get("userinfo")
    is_mobile_login = request.session.pop("mobile_login", False)

    if user and user["email"] in config.authorized:
        jwt_token = create_jwt(
            {
                "sub": user["email"],
                "name": user.get("name"),
            }
        )

        if is_mobile_login:
            return RedirectResponse(url=f"turbacz://auth-callback?token={jwt_token}")

        response = RedirectResponse(url="/auto")
        response.set_cookie(
            "access_token",
            jwt_token,
            httponly=True,
            secure=not config.security.allow_insecure_http,
            samesite="lax",
            max_age=JWT_EXPIRE_MINUTES * 60,
            path="/",
        )

        return response

    if is_mobile_login:
        return RedirectResponse(url="turbacz://auth-callback?error=unauthorized")

    return RedirectResponse(url="/")


@router.post("/logout")
async def logout(response: Response):
    response = RedirectResponse(url="/", status_code=303)
    response.delete_cookie("access_token", path="/")

    return response


@router.get("/auto")
async def main(request: Request, access_token: Optional[str] = Cookie(None)):
    user = get_current_user(access_token)

    if user:
        with open(os.path.join("static", "index.html")) as fh:
            data = fh.read()

        return Response(content=data, media_type="text/html")

    return RedirectResponse(url="/")


@router.get("/csrf-token")
async def get_csrf_token(request: Request):
    from starlette.responses import JSONResponse
    from turbacz.security import csrf_token

    token = request.cookies.get("access_token")
    if not get_current_user(token):
        raise HTTPException(status_code=401, detail="Unauthorized")
    return JSONResponse({"token": csrf_token(token)}, headers={"Cache-Control": "no-store"})
