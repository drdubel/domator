from typing import Optional

import psycopg
from fastapi import APIRouter, Cookie, Form, Request

from turbacz import auth
from turbacz.settings import config

router = APIRouter(prefix="/lights")


class ConnectionManager:
    def __init__(self):
        self._connections: dict[int, dict[str, list[tuple[int, str]]]] = {}
        self._relays: dict[int, str] = {}
        self._switches: dict[int, str] = {}
        self._init_db()
        self.create_tables()
        self.load_from_db()

    def _init_db(self):
        print(config.psql.user)
        self.conn = psycopg.connect(
            f"dbname={config.psql.dbname} user={config.psql.user} password={config.psql.password} host={config.psql.host} port={config.psql.port}"
        )
        self.cur = self.conn.cursor()

    def create_tables(self):
        self.cur.execute(
            """
            CREATE TABLE IF NOT EXISTS relays (
                id SERIAL PRIMARY KEY,
                name TEXT NOT NULL
            );
            """
        )
        self.cur.execute(
            """
            CREATE TABLE IF NOT EXISTS switches (
                id SERIAL PRIMARY KEY,
                name TEXT NOT NULL,
                buttons INTEGER NOT NULL
            );
            """
        )
        self.cur.execute(
            """
            CREATE TABLE IF NOT EXISTS connections (
                switch_id INTEGER REFERENCES switches(id),
                button_id TEXT,
                relay_id INTEGER REFERENCES relays(id),
                output_id TEXT
            );
            """
        )
        self.conn.commit()

    def save_to_db(self):
        for relay_id, relay_name in self._relays.items():
            self.cur.execute(
                """
                INSERT INTO relays (id, name)
                VALUES (%s, %s)
                ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name;
                """,
                (relay_id, relay_name),
            )

        for switch_id, switch_name in self._switches.items():
            self.cur.execute(
                """
                INSERT INTO switches (id, name, buttons)
                VALUES (%s, %s, %s)
                ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name;
                """,
                (switch_id, switch_name, len(self._connections.get(switch_id, {}))),
            )

        for switch_id, buttons in self._connections.items():
            for button_id, connections in buttons.items():
                for relay_id, output_id in connections:
                    self.cur.execute(
                        """
                        INSERT INTO connections (switch_id, button_id, relay_id, output_id)
                        VALUES (%s, %s, %s, %s);
                        """,
                        (switch_id, button_id, relay_id, output_id),
                    )
        self.conn.commit()

    def load_from_db(self):
        self.cur.execute("SELECT id, name FROM relays;")
        for relay_id, relay_name in self.cur.fetchall():
            self._relays[relay_id] = relay_name

        self.cur.execute("SELECT id, name FROM switches;")
        for switch_id, switch_name in self.cur.fetchall():
            self._switches[switch_id] = switch_name

        self.cur.execute(
            "SELECT switch_id, button_id, relay_id, output_id FROM connections;"
        )
        for switch_id, button_id, relay_id, output_id in self.cur.fetchall():
            if switch_id not in self._connections:
                self._connections[switch_id] = {}

            if button_id not in self._connections[switch_id]:
                self._connections[switch_id][button_id] = []

            self._connections[switch_id][button_id].append((relay_id, output_id))

    def add_relay(self, relay_id: int, relay_name: str):
        self._relays[relay_id] = relay_name

    def get_relay(self, relay_id: int) -> str | None:
        return self._relays.get(relay_id, None)

    def add_switch(self, switch_id: int, switch_name: str):
        self._switches[switch_id] = switch_name

    def get_switch(self, switch_id: int) -> str | None:
        return self._switches.get(switch_id, None)

    def add_connection(
        self, switch_id: int, button_id: str, relay_id: int, output_id: str
    ):
        if switch_id not in self._connections:
            self._connections[switch_id] = {}

        if button_id not in self._connections[switch_id]:
            self._connections[switch_id][button_id] = []

        self._connections[switch_id][button_id].append((relay_id, output_id))

    def get_connection(self, switch_id: int, button_id: str) -> tuple[int, str] | None:
        return self._connections.get(switch_id, {}).get(button_id, None)

    def get_all_connections(self) -> dict[int, dict[str, tuple[int, str]]]:
        return self._connections


@router.post("/add_relay")
def add_relay(
    request: Request,
    relay_id: int = Form(...),
    relay_name: str = Form(...),
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    connection_manager.add_relay(relay_id, relay_name)
    connection_manager.save_to_db()

    return {"status": "Relay added"}


@router.post("/add_switch")
def add_switch(
    request: Request,
    switch_id: int = Form(...),
    switch_name: str = Form(...),
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    connection_manager.add_switch(switch_id, switch_name)
    connection_manager.save_to_db()

    return {"status": "Switch added"}


@router.post("/add_connection")
def add_connection(
    request: Request,
    switch_id: int = Form(...),
    button_id: str = Form(...),
    relay_id: int = Form(...),
    output_id: str = Form(...),
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    connection_manager.add_connection(switch_id, button_id, relay_id, output_id)
    connection_manager.save_to_db()

    return {"status": "Connection added"}


@router.get("/get_connections")
def get_connections(
    request: Request,
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    connections = connection_manager.get_all_connections()
    return {"connections": connections}


connection_manager = ConnectionManager()
