"""FastAPI router for HA bridge configuration and apply trigger."""

import logging
from typing import Optional

from fastapi import APIRouter, Cookie, Depends, HTTPException
from fastapi.responses import JSONResponse

import turbacz.auth as auth
from turbacz.ha.apply import apply, build_lights_from_turbacz
from turbacz.ha.db import ha_db
from turbacz.ha.models import (
    HAApplyResult,
    HAArea,
    HAAreaCreate,
    HACapability,
    HACapabilityCreate,
    HADevice,
    HADeviceCreate,
    HAHome,
    HAHomeCreate,
)

logger = logging.getLogger(__name__)

ha_router = APIRouter(prefix="/api/ha", tags=["home-assistant"])


def _require_authenticated_user(access_token: Optional[str] = Cookie(None)):
    user = auth.get_current_user(access_token)
    if not user:
        raise HTTPException(status_code=401, detail="Unauthorized")


def _get_mqtt_client():
    """Return the live MQTT client.  Separated out so tests can override it."""
    # Deferred import avoids circular imports at startup.
    from turbacz.broker import mqtt  # noqa: PLC0415

    if not mqtt.client:
        raise HTTPException(status_code=503, detail="MQTT client not connected.")
    return mqtt.client


# ── Homes ─────────────────────────────────────────────────────────────────────


@ha_router.get("/homes", response_model=list[HAHome])
def list_homes(user=Depends(_require_authenticated_user)):
    return ha_db.get_homes()


@ha_router.post("/homes", response_model=HAHome, status_code=201)
def create_home(data: HAHomeCreate, user=Depends(_require_authenticated_user)):
    if not data.name.strip():
        raise HTTPException(status_code=422, detail="Home name must not be empty.")
    return ha_db.create_home(data)


@ha_router.delete("/homes/{home_id}", status_code=204)
def delete_home(home_id: str, user=Depends(_require_authenticated_user)):
    if not ha_db.delete_home(home_id):
        raise HTTPException(status_code=404, detail="Home not found.")


# ── Areas ─────────────────────────────────────────────────────────────────────


@ha_router.get("/homes/{home_id}/areas", response_model=list[HAArea])
def list_areas(home_id: str, user=Depends(_require_authenticated_user)):
    if ha_db.get_home(home_id) is None:
        raise HTTPException(status_code=404, detail="Home not found.")
    return ha_db.get_areas(home_id)


@ha_router.post("/areas", response_model=HAArea, status_code=201)
def create_area(data: HAAreaCreate, user=Depends(_require_authenticated_user)):
    if not data.name.strip():
        raise HTTPException(status_code=422, detail="Area name must not be empty.")
    if ha_db.get_home(data.home_id) is None:
        raise HTTPException(status_code=404, detail="Home not found.")
    return ha_db.create_area(data)


@ha_router.delete("/areas/{area_id}", status_code=204)
def delete_area(area_id: str, user=Depends(_require_authenticated_user)):
    if not ha_db.delete_area(area_id):
        raise HTTPException(status_code=404, detail="Area not found.")


# ── Devices ───────────────────────────────────────────────────────────────────


@ha_router.get("/areas/{area_id}/devices", response_model=list[HADevice])
def list_devices(area_id: str, user=Depends(_require_authenticated_user)):
    if ha_db.get_area(area_id) is None:
        raise HTTPException(status_code=404, detail="Area not found.")
    return ha_db.get_devices(area_id)


@ha_router.post("/devices", response_model=HADevice, status_code=201)
def create_device(data: HADeviceCreate, user=Depends(_require_authenticated_user)):
    if not data.name.strip():
        raise HTTPException(status_code=422, detail="Device name must not be empty.")
    if ha_db.get_area(data.area_id) is None:
        raise HTTPException(status_code=404, detail="Area not found.")
    return ha_db.create_device(data)


@ha_router.delete("/devices/{device_id}", status_code=204)
def delete_device(device_id: str, user=Depends(_require_authenticated_user)):
    if not ha_db.delete_device(device_id):
        raise HTTPException(status_code=404, detail="Device not found.")


# ── Capabilities ──────────────────────────────────────────────────────────────


@ha_router.get("/devices/{device_id}/capabilities", response_model=list[HACapability])
def list_capabilities(device_id: str, user=Depends(_require_authenticated_user)):
    if ha_db.get_device(device_id) is None:
        raise HTTPException(status_code=404, detail="Device not found.")
    return ha_db.get_capabilities(device_id)


@ha_router.post("/capabilities", response_model=HACapability, status_code=201)
def create_capability(data: HACapabilityCreate, user=Depends(_require_authenticated_user)):
    if not data.name.strip():
        raise HTTPException(status_code=422, detail="Capability name must not be empty.")
    if ha_db.get_device(data.device_id) is None:
        raise HTTPException(status_code=404, detail="Device not found.")
    return ha_db.create_capability(data)


@ha_router.delete("/capabilities/{cap_id}", status_code=204)
def delete_capability(cap_id: str, user=Depends(_require_authenticated_user)):
    if not ha_db.delete_capability(cap_id):
        raise HTTPException(status_code=404, detail="Capability not found.")


# ── Full tree ─────────────────────────────────────────────────────────────────


@ha_router.get("/tree", response_model=list[HAHome])
def get_tree(user=Depends(_require_authenticated_user)):
    """Return the auto-derived entity tree built from Turbacz relay outputs."""
    return build_lights_from_turbacz()


# ── Apply ─────────────────────────────────────────────────────────────────────


@ha_router.post("/apply", response_model=HAApplyResult)
def trigger_apply(
    user=Depends(_require_authenticated_user),
    mqtt_client=Depends(_get_mqtt_client),
):
    """
    Publish (or clear) retained HA MQTT Discovery configs for all named
    light outputs registered in Turbacz.  Idempotent — safe to call
    multiple times.
    """
    result = apply(mqtt_client, ha_db)

    if result.errors:
        return JSONResponse(status_code=207, content=result.model_dump())

    return result
