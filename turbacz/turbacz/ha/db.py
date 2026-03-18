"""PostgreSQL persistence layer for HA bridge config."""

from __future__ import annotations

import uuid

import psycopg

from turbacz.ha.models import (
    HAArea,
    HAAreaCreate,
    HACapability,
    HACapabilityCreate,
    HADevice,
    HADeviceCreate,
    HAHome,
    HAHomeCreate,
)
from turbacz.settings import config


def _connect() -> psycopg.Connection:
    return psycopg.connect(
        f"dbname={config.psql.dbname} user={config.psql.user} "
        f"password={config.psql.password} host={config.psql.host} port={config.psql.port}"
    )


class HADatabase:
    """Manages HA config tables and applied-topic tracking in PostgreSQL."""

    def __init__(self):
        self.conn = _connect()
        self._create_tables()

    def _create_tables(self) -> None:
        with self.conn.cursor() as cur:
            cur.execute(
                """
                CREATE TABLE IF NOT EXISTS ha_homes (
                    id   TEXT PRIMARY KEY,
                    name TEXT NOT NULL
                );
                """
            )
            cur.execute(
                """
                CREATE TABLE IF NOT EXISTS ha_areas (
                    id      TEXT PRIMARY KEY,
                    home_id TEXT NOT NULL REFERENCES ha_homes(id) ON DELETE CASCADE,
                    name    TEXT NOT NULL
                );
                """
            )
            cur.execute(
                """
                CREATE TABLE IF NOT EXISTS ha_devices (
                    id      TEXT PRIMARY KEY,
                    area_id TEXT NOT NULL REFERENCES ha_areas(id) ON DELETE CASCADE,
                    name    TEXT NOT NULL
                );
                """
            )
            cur.execute(
                """
                CREATE TABLE IF NOT EXISTS ha_capabilities (
                    id              TEXT PRIMARY KEY,
                    device_id       TEXT NOT NULL REFERENCES ha_devices(id) ON DELETE CASCADE,
                    capability_type TEXT NOT NULL
                                        CHECK (capability_type IN ('light', 'cover', 'climate')),
                    name            TEXT NOT NULL
                );
                """
            )
            cur.execute(
                """
                CREATE TABLE IF NOT EXISTS ha_applied_topics (
                    topic         TEXT PRIMARY KEY,
                    capability_id TEXT NOT NULL
                );
                """
            )
        self.conn.commit()

    # ── Homes ─────────────────────────────────────────────────────────────────

    def create_home(self, data: HAHomeCreate) -> HAHome:
        home_id = str(uuid.uuid4())
        with self.conn.cursor() as cur:
            cur.execute(
                "INSERT INTO ha_homes (id, name) VALUES (%s, %s);",
                (home_id, data.name),
            )
        self.conn.commit()
        return HAHome(id=home_id, name=data.name)

    def get_homes(self) -> list[HAHome]:
        with self.conn.cursor() as cur:
            cur.execute("SELECT id, name FROM ha_homes ORDER BY name;")
            rows = cur.fetchall()
        return [HAHome(id=r[0], name=r[1]) for r in rows]

    def get_home(self, home_id: str) -> HAHome | None:
        with self.conn.cursor() as cur:
            cur.execute("SELECT id, name FROM ha_homes WHERE id = %s;", (home_id,))
            row = cur.fetchone()
        if row is None:
            return None
        return HAHome(id=row[0], name=row[1])

    def delete_home(self, home_id: str) -> bool:
        with self.conn.cursor() as cur:
            cur.execute("DELETE FROM ha_homes WHERE id = %s;", (home_id,))
            deleted = cur.rowcount > 0
        self.conn.commit()
        return deleted

    # ── Areas ─────────────────────────────────────────────────────────────────

    def create_area(self, data: HAAreaCreate) -> HAArea:
        area_id = str(uuid.uuid4())
        with self.conn.cursor() as cur:
            cur.execute(
                "INSERT INTO ha_areas (id, home_id, name) VALUES (%s, %s, %s);",
                (area_id, data.home_id, data.name),
            )
        self.conn.commit()
        return HAArea(id=area_id, home_id=data.home_id, name=data.name)

    def get_areas(self, home_id: str) -> list[HAArea]:
        with self.conn.cursor() as cur:
            cur.execute(
                "SELECT id, home_id, name FROM ha_areas WHERE home_id = %s ORDER BY name;",
                (home_id,),
            )
            rows = cur.fetchall()
        return [HAArea(id=r[0], home_id=r[1], name=r[2]) for r in rows]

    def get_area(self, area_id: str) -> HAArea | None:
        with self.conn.cursor() as cur:
            cur.execute("SELECT id, home_id, name FROM ha_areas WHERE id = %s;", (area_id,))
            row = cur.fetchone()
        if row is None:
            return None
        return HAArea(id=row[0], home_id=row[1], name=row[2])

    def delete_area(self, area_id: str) -> bool:
        with self.conn.cursor() as cur:
            cur.execute("DELETE FROM ha_areas WHERE id = %s;", (area_id,))
            deleted = cur.rowcount > 0
        self.conn.commit()
        return deleted

    # ── Devices ───────────────────────────────────────────────────────────────

    def create_device(self, data: HADeviceCreate) -> HADevice:
        device_id = str(uuid.uuid4())
        with self.conn.cursor() as cur:
            cur.execute(
                "INSERT INTO ha_devices (id, area_id, name) VALUES (%s, %s, %s);",
                (device_id, data.area_id, data.name),
            )
        self.conn.commit()
        return HADevice(id=device_id, area_id=data.area_id, name=data.name)

    def get_devices(self, area_id: str) -> list[HADevice]:
        with self.conn.cursor() as cur:
            cur.execute(
                "SELECT id, area_id, name FROM ha_devices WHERE area_id = %s ORDER BY name;",
                (area_id,),
            )
            rows = cur.fetchall()
        return [HADevice(id=r[0], area_id=r[1], name=r[2]) for r in rows]

    def get_device(self, device_id: str) -> HADevice | None:
        with self.conn.cursor() as cur:
            cur.execute("SELECT id, area_id, name FROM ha_devices WHERE id = %s;", (device_id,))
            row = cur.fetchone()
        if row is None:
            return None
        return HADevice(id=row[0], area_id=row[1], name=row[2])

    def delete_device(self, device_id: str) -> bool:
        with self.conn.cursor() as cur:
            cur.execute("DELETE FROM ha_devices WHERE id = %s;", (device_id,))
            deleted = cur.rowcount > 0
        self.conn.commit()
        return deleted

    # ── Capabilities ──────────────────────────────────────────────────────────

    def create_capability(self, data: HACapabilityCreate) -> HACapability:
        cap_id = str(uuid.uuid4())
        with self.conn.cursor() as cur:
            cur.execute(
                """
                INSERT INTO ha_capabilities (id, device_id, capability_type, name)
                VALUES (%s, %s, %s, %s);
                """,
                (cap_id, data.device_id, data.capability_type.value, data.name),
            )
        self.conn.commit()
        return HACapability(
            id=cap_id,
            device_id=data.device_id,
            capability_type=data.capability_type,
            name=data.name,
        )

    def get_capabilities(self, device_id: str) -> list[HACapability]:
        with self.conn.cursor() as cur:
            cur.execute(
                """
                SELECT id, device_id, capability_type, name
                FROM ha_capabilities
                WHERE device_id = %s
                ORDER BY name;
                """,
                (device_id,),
            )
            rows = cur.fetchall()
        return [
            HACapability(id=r[0], device_id=r[1], capability_type=r[2], name=r[3])
            for r in rows
        ]

    def get_capability(self, cap_id: str) -> HACapability | None:
        with self.conn.cursor() as cur:
            cur.execute(
                "SELECT id, device_id, capability_type, name FROM ha_capabilities WHERE id = %s;",
                (cap_id,),
            )
            row = cur.fetchone()
        if row is None:
            return None
        return HACapability(id=row[0], device_id=row[1], capability_type=row[2], name=row[3])

    def delete_capability(self, cap_id: str) -> bool:
        with self.conn.cursor() as cur:
            cur.execute("DELETE FROM ha_capabilities WHERE id = %s;", (cap_id,))
            deleted = cur.rowcount > 0
        self.conn.commit()
        return deleted

    # ── Full tree ─────────────────────────────────────────────────────────────

    def get_full_tree(self) -> list[HAHome]:
        """Return the full home/area/device/capability tree in one pass."""
        with self.conn.cursor() as cur:
            cur.execute(
                """
                SELECT
                    h.id   AS home_id,   h.name AS home_name,
                    ar.id  AS area_id,   ar.name AS area_name,
                    d.id   AS dev_id,    d.name AS dev_name,
                    c.id   AS cap_id,    c.capability_type, c.name AS cap_name
                FROM ha_homes h
                LEFT JOIN ha_areas       ar ON ar.home_id  = h.id
                LEFT JOIN ha_devices      d ON d.area_id   = ar.id
                LEFT JOIN ha_capabilities c ON c.device_id = d.id
                ORDER BY h.name, ar.name, d.name, c.name;
                """
            )
            rows = cur.fetchall()

        homes: dict[str, HAHome] = {}
        areas: dict[str, HAArea] = {}
        devices: dict[str, HADevice] = {}

        for row in rows:
            home_id, home_name, area_id, area_name, dev_id, dev_name, cap_id, cap_type, cap_name = row

            if home_id not in homes:
                homes[home_id] = HAHome(id=home_id, name=home_name)

            if area_id and area_id not in areas:
                area = HAArea(id=area_id, home_id=home_id, name=area_name)
                areas[area_id] = area
                homes[home_id].areas.append(area)

            if dev_id and dev_id not in devices:
                device = HADevice(id=dev_id, area_id=area_id, name=dev_name)
                devices[dev_id] = device
                areas[area_id].devices.append(device)

            if cap_id:
                cap = HACapability(
                    id=cap_id, device_id=dev_id, capability_type=cap_type, name=cap_name
                )
                devices[dev_id].capabilities.append(cap)

        return list(homes.values())

    # ── Applied-topic tracking ────────────────────────────────────────────────

    def get_applied_topics(self) -> dict[str, str]:
        """Return {topic: capability_id} for all currently applied discovery topics."""
        with self.conn.cursor() as cur:
            cur.execute("SELECT topic, capability_id FROM ha_applied_topics;")
            rows = cur.fetchall()
        return {r[0]: r[1] for r in rows}

    def upsert_applied_topic(self, topic: str, capability_id: str) -> None:
        with self.conn.cursor() as cur:
            cur.execute(
                """
                INSERT INTO ha_applied_topics (topic, capability_id)
                VALUES (%s, %s)
                ON CONFLICT (topic) DO UPDATE SET capability_id = EXCLUDED.capability_id;
                """,
                (topic, capability_id),
            )
        self.conn.commit()

    def delete_applied_topic(self, topic: str) -> None:
        with self.conn.cursor() as cur:
            cur.execute("DELETE FROM ha_applied_topics WHERE topic = %s;", (topic,))
        self.conn.commit()

    def delete_applied_topics_for_capability(self, capability_id: str) -> list[str]:
        """Remove tracking rows for a capability and return the topic names."""
        with self.conn.cursor() as cur:
            cur.execute(
                "DELETE FROM ha_applied_topics WHERE capability_id = %s RETURNING topic;",
                (capability_id,),
            )
            rows = cur.fetchall()
        self.conn.commit()
        return [r[0] for r in rows]


ha_db = HADatabase()
