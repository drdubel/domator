from typing import Sequence
from pydantic import BaseModel
from pydantic_settings import BaseSettings, SettingsConfigDict


class MQTTSettings(BaseModel):
    host: str = "127.0.0.1"
    port: int = 1883
    keepalive: int = 60
    username: str = "turbacz"
    password: str


class OAuthSettings(BaseModel):
    configuration_url: str = (
        "https://accounts.google.com/.well-known/openid-configuration"
    )
    client_id: str
    client_secret: str


class Settings(BaseSettings):
    mqtt: MQTTSettings
    oauth: OAuthSettings
    authorized_users: Sequence[str]
    cookies_path: str = "data/cookies.pickle"
    victoria_metrics_url: str = "http://127.0.0.1:8428"
    session_secret: str

    model_config = SettingsConfigDict(
        cli_parse_args=True,
        env_file=(".env", ".env.secrets"),
        env_nested_delimiter="_",
        env_nested_max_split=1,
        env_file_encoding="utf-8",
    )


settings = Settings()
