from typing import Optional

from pydantic import BaseModel, Field
from pydantic_settings import (
    BaseSettings,
    PydanticBaseSettingsSource,
    SettingsConfigDict,
    TomlConfigSettingsSource,
)


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
    forwarded_allow_ips: Optional[str] = None


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
    sentry_traces_sample_rate: float = Field(default=0.1, ge=0, le=1)
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


class SecuritySettings(BaseModel):
    # The configured OIDC redirect origin/host is also trusted. No forwarded
    # header is used to derive these allowlists.
    allowed_hosts: list[str] = ["localhost", "127.0.0.1", "::1", "turbacz"]
    allowed_origins: list[str] = ["https://localhost", "https://127.0.0.1"]
    allow_insecure_http: bool = False
    auth_requests_per_minute: int = Field(default=30, ge=1)
    uploads_per_minute: int = Field(default=5, ge=1)
    ws_handshakes_per_minute: int = Field(default=60, ge=1)
    ws_connections_per_user: int = Field(default=8, ge=1)
    ws_connections_per_ip: int = Field(default=16, ge=1)
    ws_connections_total: int = Field(default=128, ge=1)
    ws_messages_per_minute: int = Field(default=240, ge=1)
    ws_max_bytes: int = Field(default=65536, ge=1024, le=1048576)
    ws_max_items: int = Field(default=1024, ge=1, le=4096)
    ws_idle_seconds: float = Field(default=300, gt=0)
    ws_send_timeout: float = Field(default=5, gt=0)


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
    security: SecuritySettings = SecuritySettings()
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
