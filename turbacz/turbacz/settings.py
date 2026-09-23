from typing import Optional

from pydantic import BaseModel
from pydantic_settings import BaseSettings, PydanticBaseSettingsSource, SettingsConfigDict, TomlConfigSettingsSource


class OIDCSettings(BaseModel):
    provider: str = "google"
    client_id: str
    client_secret: str
    server_metadata_url: str = "https://accounts.google.com/.well-known/openid-configuration"
    allow_insecure_http: bool = False
    redirect_uri: Optional[str] = None
    token_endpoint_auth_method: Optional[str] = None


class MQTTServerSettings(BaseModel):
    host: str = "127.0.0.1"
    port: int = 1883
    username: str = "turbacz"
    password: str


class ServerSettings(BaseModel):
    host: str = "127.0.0.1"
    port: int = 8000


class PSQLSettings(BaseModel):
    dbname: str = "turbacz"
    user: str = "turbacz"
    password: str = "turbacz"
    host: str = "127.0.0.1"
    port: int = 5432


class Monitoring(BaseModel):
    send_metrics: bool = True
    metrics: str = "http://127.0.0.1:8428"
    labels: dict[str, str] = {}
    sentry_dsn: Optional[str] = None
    collect_host_metrics: bool = True
    # "auto" detects common containers; it can be overridden with "host" or
    # "container". The label prevents container-visible values being mistaken
    # for physical-host measurements in Grafana.
    host_metrics_scope: str = "auto"


class FirmwareSettings(BaseModel):
    """OTA images served to the microcontrollers.

    These binaries embed WiFi and MQTT credentials, so they must never sit
    under the public ``static/`` mount. They live in their own directory and
    are handed out only to a logged-in browser session or to a device that
    presents ``token`` in the ``X-Firmware-Token`` header.
    """

    directory: str = "firmware"
    # Empty means devices cannot download at all -- deny by default, so a
    # missing config value can never reopen anonymous access.
    token: str = ""


class HASettings(BaseModel):
    """Home Assistant MQTT Discovery bridge."""

    enabled: bool = False
    discovery_prefix: str = "homeassistant"
    base_topic: str = "domator"
    resync_interval: int = 60


class TurbaczSettings(BaseSettings):
    authorized: set[str] = set()
    jwt_secret: str = ""
    session_secret: str = ""
    mqtt: MQTTServerSettings = MQTTServerSettings(password="")
    oidc: OIDCSettings = OIDCSettings(client_id="", client_secret="")
    monitoring: Monitoring = Monitoring()
    server: ServerSettings = ServerSettings()
    psql: PSQLSettings = PSQLSettings()
    ha: HASettings = HASettings()
    firmware: FirmwareSettings = FirmwareSettings()
    use_mqtt: bool = True

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
