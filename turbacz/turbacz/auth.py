from datetime import datetime, timedelta, timezone

from fastapi import WebSocket
from jose import JWTError, jwt

from turbacz.data.secrets import JWT_SECRET

JWT_ALG = "HS256"
JWT_EXPIRE_MINUTES = 60 * 24 * 14  # 14 days


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
        return jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALG])

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
