"""Idempotent apply pipeline: publish/clear HA MQTT Discovery topics."""

import json
import logging

from turbacz.ha import topics as T
from turbacz.ha.db import HADatabase
from turbacz.ha.discovery import build_discovery_payload
from turbacz.ha.models import HAApplyResult, HACapability, CapabilityType

logger = logging.getLogger(__name__)


def _discovery_topic_for_cap(cap: HACapability, home_id: str, area_id: str) -> str:
    uid = T.unique_id(home_id, area_id, cap.device_id, cap.capability_type.value)
    return T.discovery_topic(cap.capability_type.value, uid)


def apply(mqtt_client, db: HADatabase) -> HAApplyResult:
    """
    Publish retained HA discovery configs for all capabilities in the DB and
    clear (empty retained payload) any previously applied topics that no
    longer correspond to a capability.

    Parameters
    ----------
    mqtt_client:
        An object with a ``publish(topic, payload, qos, retain)`` method
        (compatible with the gmqtt / fastapi-mqtt client).
    db:
        An open :class:`HADatabase` instance.

    Returns
    -------
    HAApplyResult
        Lists of published/removed topics and any non-fatal errors.
    """
    result = HAApplyResult()

    # ── 1. Load full tree ────────────────────────────────────────────────────
    homes = db.get_full_tree()

    # Build the set of desired (topic → capability_id) mappings
    desired: dict[str, str] = {}  # discovery_topic → cap.id

    for home in homes:
        for area in home.areas:
            for device in area.devices:
                for cap in device.capabilities:
                    dtopic = _discovery_topic_for_cap(cap, home.id, area.id)
                    desired[dtopic] = cap.id

    # ── 2. Determine what to publish and what to remove ──────────────────────
    currently_applied = db.get_applied_topics()  # {topic: cap_id}

    to_publish = set(desired.keys())
    to_remove = set(currently_applied.keys()) - to_publish

    # ── 3. Publish discovery payloads (retained) ─────────────────────────────
    for home in homes:
        # Publish availability birth message
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
                        mqtt_client.publish(
                            dtopic,
                            json.dumps(payload),
                            qos=1,
                            retain=True,
                        )
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


def get_command_topics(db: HADatabase) -> list[str]:
    """Return all command topics that should be subscribed to."""
    homes = db.get_full_tree()
    topics: list[str] = []
    for home in homes:
        for area in home.areas:
            for device in area.devices:
                for cap in device.capabilities:
                    topics.append(
                        T.command_topic(home.id, area.id, device.id, cap.capability_type.value)
                    )
                    if cap.capability_type == CapabilityType.cover:
                        topics.append(
                            T.cover_set_position_topic(home.id, area.id, device.id)
                        )
                    elif cap.capability_type == CapabilityType.climate:
                        topics.append(
                            T.climate_mode_command_topic(home.id, area.id, device.id)
                        )
                        topics.append(
                            T.climate_target_temp_command_topic(home.id, area.id, device.id)
                        )
    return topics
