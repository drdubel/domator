"""Idempotent apply pipeline: publish/clear HA MQTT Discovery topics.

Lights are derived automatically from the Turbacz relay outputs and sections
(no manual Home Assistant configuration required).  Heating and blinds are
not yet implemented.
"""

import json
import logging

from turbacz.ha import topics as T
from turbacz.ha.db import HADatabase
from turbacz.ha.discovery import build_discovery_payload
from turbacz.ha.models import (
    CapabilityType,
    HAApplyResult,
    HAArea,
    HACapability,
    HADevice,
    HAHome,
)

logger = logging.getLogger(__name__)

_DEFAULT_HOME_ID = "home"
_DEFAULT_HOME_NAME = "Home"


# ── Auto-derivation from Turbacz relay outputs ────────────────────────────────


def build_lights_from_turbacz(cm=None) -> list[HAHome]:
    """Auto-derive HA light entities from Turbacz relay outputs and sections.

    Each named relay output becomes one HA ``light`` entity.  Outputs whose
    name starts with ``"Output "`` (the auto-generated default) are skipped.
    Sections map to HA areas; unnamed outputs fall into an "Uncategorized"
    area.

    Parameters
    ----------
    cm:
        Optional :class:`ConnectionManager` instance.  When *None* the
        global singleton is used.  Pass an explicit object in unit tests
        to avoid real database access.
    """
    if cm is None:
        from turbacz.connection_manager import connection_manager as cm

    sections: dict[int, str] = cm.get_sections() or {}
    named_outputs: dict[int, dict] = cm.get_named_outputs()

    # Group outputs by section_id
    section_outputs: dict[int, list] = {}
    for relay_id, relay_outputs in named_outputs.items():
        for output_id, (name, section_id, _idx, _auto) in relay_outputs.items():
            section_outputs.setdefault(section_id, []).append((relay_id, output_id, name))

    def _make_device(relay_id: int, output_id: str, name: str, area_id: str) -> HADevice:
        device_id = f"r{relay_id}_{output_id}"
        cap = HACapability(
            id=f"{device_id}_light",
            device_id=device_id,
            capability_type=CapabilityType.light,
            name=name,
        )
        return HADevice(id=device_id, area_id=area_id, name=name, capabilities=[cap])

    areas: list[HAArea] = []

    for section_id, section_name in sections.items():
        area_id = f"s{section_id}"
        devices = [
            _make_device(relay_id, output_id, name, area_id)
            for relay_id, output_id, name in section_outputs.get(section_id, [])
        ]
        if devices:
            areas.append(
                HAArea(id=area_id, home_id=_DEFAULT_HOME_ID, name=section_name, devices=devices)
            )

    # Outputs not assigned to any known section
    known_section_ids = set(sections.keys())
    uncategorized = [
        (relay_id, output_id, name)
        for sec_id, items in section_outputs.items()
        if sec_id not in known_section_ids
        for relay_id, output_id, name in items
    ]
    if uncategorized:
        area_id = "s0"
        areas.append(
            HAArea(
                id=area_id,
                home_id=_DEFAULT_HOME_ID,
                name="Uncategorized",
                devices=[
                    _make_device(relay_id, output_id, name, area_id)
                    for relay_id, output_id, name in uncategorized
                ],
            )
        )

    if not areas:
        return []
    return [HAHome(id=_DEFAULT_HOME_ID, name=_DEFAULT_HOME_NAME, areas=areas)]


# ── Shared helpers ────────────────────────────────────────────────────────────


def _discovery_topic_for_cap(cap: HACapability, home_id: str, area_id: str) -> str:
    uid = T.unique_id(home_id, area_id, cap.device_id, cap.capability_type.value)
    return T.discovery_topic(cap.capability_type.value, uid)


# ── Apply pipeline ────────────────────────────────────────────────────────────


def apply(mqtt_client, db: HADatabase, cm=None) -> HAApplyResult:
    """Publish retained HA discovery configs for all named light outputs and
    clear (empty retained payload) any previously applied topics that no
    longer correspond to a current output.

    Parameters
    ----------
    mqtt_client:
        An object with a ``publish(topic, payload, qos, retain)`` method
        (compatible with the gmqtt / fastapi-mqtt client).
    db:
        An open :class:`HADatabase` instance used for applied-topic tracking.
    cm:
        Optional connection-manager instance; defaults to the global
        singleton.  Pass a mock in unit tests to avoid real database access.

    Returns
    -------
    HAApplyResult
        Lists of published/removed topics and any non-fatal errors.
    """
    result = HAApplyResult()

    # ── 1. Auto-derive light entities from Turbacz outputs ───────────────────
    homes = build_lights_from_turbacz(cm)

    desired: dict[str, str] = {}  # discovery_topic → cap.id
    for home in homes:
        for area in home.areas:
            for device in area.devices:
                for cap in device.capabilities:
                    dtopic = _discovery_topic_for_cap(cap, home.id, area.id)
                    desired[dtopic] = cap.id

    # ── 2. Diff against previously applied topics ────────────────────────────
    currently_applied = db.get_applied_topics()
    to_remove = set(currently_applied.keys()) - set(desired.keys())

    # ── 3. Publish discovery payloads (retained) ─────────────────────────────
    for home in homes:
        avail_topic = T.availability_topic(home.id)
        try:
            mqtt_client.publish(avail_topic, "online", qos=1, retain=True)
        except Exception as exc:
            logger.error("Failed to publish availability for home %s: %s", home.id, exc)
            result.errors.append(f"availability {home.id}: {exc}")

        for area in home.areas:
            for device in area.devices:
                for cap in device.capabilities:
                    dtopic = _discovery_topic_for_cap(cap, home.id, area.id)
                    try:
                        payload = build_discovery_payload(cap, home.id, home.name, area.id)
                        mqtt_client.publish(dtopic, json.dumps(payload), qos=1, retain=True)
                        db.upsert_applied_topic(dtopic, cap.id)
                        result.published.append(dtopic)
                        logger.info("Published discovery: %s", dtopic)
                    except Exception as exc:
                        logger.error("Failed to publish discovery %s: %s", dtopic, exc)
                        result.errors.append(f"{dtopic}: {exc}")

    # ── 4. Clear removed entities (empty retained payload) ───────────────────
    for topic in to_remove:
        try:
            mqtt_client.publish(topic, "", qos=1, retain=True)
            db.delete_applied_topic(topic)
            result.removed.append(topic)
            logger.info("Cleared discovery: %s", topic)
        except Exception as exc:
            logger.error("Failed to clear discovery %s: %s", topic, exc)
            result.errors.append(f"clear {topic}: {exc}")

    return result


def get_command_topics(cm=None) -> list[str]:
    """Return all HA light command topics derived from Turbacz relay outputs."""
    topics: list[str] = []
    for home in build_lights_from_turbacz(cm):
        for area in home.areas:
            for device in area.devices:
                for cap in device.capabilities:
                    topics.append(
                        T.command_topic(home.id, area.id, device.id, cap.capability_type.value)
                    )
    return topics
