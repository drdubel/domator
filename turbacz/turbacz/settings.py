from typing import Optional

from pydantic import BaseModel, HttpUrl
from pydantic_settings import (BaseSettings, PydanticBaseSettingsSource,
                               SettingsConfigDict, TomlConfigSettingsSource)


class OIDCSettings(BaseModel):
    provider: str = "google"
    client_id: str
    client_secret: str
    server_metadata_url: HttpUrl = (
        "https://accounts.google.com/.well-known/openid-configuration"
    )


class MQTTServerSettings(BaseModel):
    host: str = "127.0.0.1"
    port: int = 1883
    username: str = "turbacz"
    password: str


class ServerSettings(BaseModel):
    host: str = "127.0.0.1"
    port: int = 8000


class TurbaczSettings(BaseSettings):
    authorized: set[str]
    jwt_secret: str
    mqtt: MQTTServerSettings
    oidc: OIDCSettings
    prometheus: HttpUrl = "http://127.0.0.1:8248"
    sentry_dsn: Optional[HttpUrl] = None
    server: ServerSettings

    model_config = SettingsConfigDict(toml_file="turbacz.toml")

    @classmethod
    def settings_customise_sources(
        cls,
        settings_cls: type[BaseSettings],
        init_settings: PydanticBaseSettingsSource,
        env_settings: PydanticBaseSettingsSource,
        dotenv_settings: PydanticBaseSettingsSource,
        file_secret_settings: PydanticBaseSettingsSource,
    ) -> tuple[PydanticBaseSettingsSource, ...]:
        return (TomlConfigSettingsSource(settings_cls),)


config = TurbaczSettings()
