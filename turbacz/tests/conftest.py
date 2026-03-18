"""
Stub out heavy dependencies (psycopg, pydantic-settings/turbacz config)
before any turbacz module is imported, so unit tests run without a
database or a turbacz.toml file.
"""

import sys
from types import ModuleType
from unittest.mock import MagicMock


# ── Stub psycopg ──────────────────────────────────────────────────────────────
psycopg_stub = ModuleType("psycopg")
psycopg_stub.connect = MagicMock()
sys.modules.setdefault("psycopg", psycopg_stub)


# ── Stub pydantic_settings ────────────────────────────────────────────────────
ps_stub = ModuleType("pydantic_settings")
ps_stub.BaseSettings = MagicMock
ps_stub.PydanticBaseSettingsSource = object
ps_stub.SettingsConfigDict = MagicMock(return_value={})
ps_stub.TomlConfigSettingsSource = MagicMock()
sys.modules.setdefault("pydantic_settings", ps_stub)
sys.modules.setdefault("pydantic_settings.providers", ModuleType("pydantic_settings.providers"))
toml_stub = ModuleType("pydantic_settings.providers.toml")
toml_stub.TomlConfigSettingsSource = MagicMock()
sys.modules.setdefault("pydantic_settings.providers.toml", toml_stub)


# ── Minimal turbacz.settings.config stub ─────────────────────────────────────
settings_mod = ModuleType("turbacz.settings")


class _PSQL:
    dbname = "turbacz"
    user = "turbacz"
    password = "turbacz"
    host = "127.0.0.1"
    port = 5432


class _MQTT:
    host = "127.0.0.1"
    port = 1883
    username = "turbacz"
    password = ""


class _HA:
    enabled = True
    discovery_prefix = "homeassistant"
    base_topic = "domator"


class _Config:
    psql = _PSQL()
    mqtt = _MQTT()
    ha = _HA()
    jwt_secret = "test-secret"
    session_secret = ""
    authorized: set = set()


settings_mod.config = _Config()
sys.modules["turbacz.settings"] = settings_mod
