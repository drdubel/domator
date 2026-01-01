from typing import Optional

import psycopg
from fastapi import APIRouter, Cookie, Form, Request
from fastapi.responses import JSONResponse

from turbacz import auth
from turbacz.settings import config

router = APIRouter(prefix="/lights")


class ConnectionManager:
    def __init__(self):
        self._connections: dict[int, dict[str, list[tuple[int, str]]]] = {}
        self._relays: dict[int, str] = {}
        self._switches: dict[int, tuple[str, int]] = {}
        self._outputs: dict[int, dict[int, str]] = {}
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
                id BIGINT PRIMARY KEY,
                name TEXT NOT NULL
            );
            """
        )
        self.cur.execute(
            """
            CREATE TABLE IF NOT EXISTS outputs (
                relay_id BIGINT REFERENCES relays(id),
                output_id TEXT NOT NULL,
                name TEXT NOT NULL
            );
            """
        )
        self.cur.execute(
            """
            CREATE TABLE IF NOT EXISTS switches (
                id BIGINT PRIMARY KEY,
                name TEXT NOT NULL,
                buttons INTEGER NOT NULL
            );
            """
        )
        self.cur.execute(
            """
            CREATE TABLE IF NOT EXISTS connections (
                switch_id BIGINT REFERENCES switches(id),
                button_id TEXT,
                relay_id BIGINT REFERENCES relays(id),
                output_id TEXT
            );
            """
        )
        self.conn.commit()

    def save_to_db(self):
        self.cur.execute("DELETE FROM outputs;")
        self.cur.execute("DELETE FROM connections;")
        self.cur.execute("DELETE FROM relays;")
        self.cur.execute("DELETE FROM switches;")

        for relay_id, relay_name in self._relays.items():
            self.cur.execute(
                """
                INSERT INTO relays (id, name)
                VALUES (%s, %s)
                ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name;
                """,
                (relay_id, relay_name),
            )

        for relay_id, outputs in self._outputs.items():
            for output_id, output_name in outputs.items():
                self.cur.execute(
                    """
                    INSERT INTO outputs (relay_id, output_id, name)
                    VALUES (%s, %s, %s)
                    ON CONFLICT DO NOTHING;
                    """,
                    (relay_id, output_id, output_name),
                )

        for switch_id, switch in self._switches.items():
            self.cur.execute(
                """
                INSERT INTO switches (id, name, buttons)
                VALUES (%s, %s, %s)
                ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name;
                """,
                (switch_id, switch[0], switch[1]),
            )

        for switch_id, buttons in self._connections.items():
            for button_id, connections in buttons.items():
                for relay_id, output_id in connections:
                    self.cur.execute(
                        """
                        INSERT INTO connections (switch_id, button_id, relay_id, output_id)
                        VALUES (%s, %s, %s, %s) ON CONFLICT DO NOTHING;
                        """,
                        (switch_id, button_id, relay_id, output_id),
                    )
        self.conn.commit()

    def load_from_db(self):
        self.cur.execute("SELECT id, name FROM relays;")
        for relay_id, relay_name in self.cur.fetchall():
            self._relays[relay_id] = relay_name

        self.cur.execute("SELECT id, name, buttons FROM switches;")
        for switch_id, switch_name, buttons in self.cur.fetchall():
            self._switches[switch_id] = (switch_name, buttons)

        self.cur.execute("SELECT relay_id, output_id, name FROM outputs;")
        for relay_id, output_id, output_name in self.cur.fetchall():
            if relay_id not in self._outputs:
                self._outputs[relay_id] = {}
            self._outputs[relay_id][output_id] = output_name

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

    def get_relays(self) -> dict[int, str] | None:
        return self._relays

    def rename_relay(self, relay_id: int, relay_name: str):
        if relay_id in self._relays:
            self._relays[relay_id] = relay_name

    def remove_relay(self, relay_id: int):
        if relay_id in self._relays:
            del self._relays[relay_id]

        if relay_id in self._outputs:
            del self._outputs[relay_id]

        for switch_id in list(self._connections.keys()):
            for button_id in list(self._connections[switch_id].keys()):
                self._connections[switch_id][button_id] = [
                    conn
                    for conn in self._connections[switch_id][button_id]
                    if conn[0] != relay_id
                ]
                if not self._connections[switch_id][button_id]:
                    del self._connections[switch_id][button_id]
            if not self._connections[switch_id]:
                del self._connections[switch_id]

    def add_switch(self, switch_id: int, switch_name: str, buttons: int = 3):
        self._switches[switch_id] = (switch_name, buttons)

    def get_switches(self) -> dict[int, tuple[str, int]] | None:
        return self._switches

    def rename_switch(self, switch_id: int, switch_name: str, buttons: int):
        if switch_id in self._switches:
            self._switches[switch_id] = (
                switch_name,
                buttons,
            )

    def remove_switch(self, switch_id: int):
        if switch_id in self._switches:
            del self._switches[switch_id]

        if switch_id in self._connections:
            del self._connections[switch_id]

    def add_output(self, relay_id: int, output_id: str, output_name: str):
        self._outputs.setdefault(relay_id, {})[output_id] = output_name

    def name_output(self, relay_id: int, output_id: str, output_name: str):
        self._outputs.setdefault(relay_id, {})[output_id] = output_name

    def get_outputs(self) -> dict[int, dict[str, str]] | None:
        return self._outputs

    def add_connection(
        self, switch_id: int, button_id: str, relay_id: int, output_id: str
    ):
        if switch_id not in self._connections:
            self._connections[switch_id] = {}

        if button_id not in self._connections[switch_id]:
            self._connections[switch_id][button_id] = []

        self._connections[switch_id][button_id].append((relay_id, output_id))

    def remove_connection(
        self, switch_id: int, button_id: str, relay_id: int, output_id: str
    ):
        if switch_id in self._connections and button_id in self._connections[switch_id]:
            connection = (relay_id, output_id)
            if connection in self._connections[switch_id][button_id]:
                self._connections[switch_id][button_id].remove(connection)
                if not self._connections[switch_id][button_id]:
                    del self._connections[switch_id][button_id]
                if not self._connections[switch_id]:
                    del self._connections[switch_id]

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
    for i in range(8):
        connection_manager.add_output(relay_id, chr(97 + i), f"Output {i + 1}")

    connection_manager.save_to_db()

    return {"status": "Relay added"}


@router.post("/name_output")
def add_output(
    request: Request,
    relay_id: int = Form(...),
    output_id: str = Form(...),
    output_name: str = Form(...),
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    connection_manager.name_output(relay_id, output_id, output_name)
    connection_manager.save_to_db()

    return {"status": "Output added"}


@router.post("/rename_relay")
def rename_relay(
    request: Request,
    relay_id: int = Form(...),
    relay_name: str = Form(...),
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    connection_manager.rename_relay(relay_id, relay_name)
    connection_manager.save_to_db()

    return {"status": "Relay renamed"}


@router.post("/rename_switch")
def rename_switch(
    request: Request,
    switch_id: int = Form(...),
    switch_name: str = Form(...),
    buttons: int = Form(...),
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    connection_manager.rename_switch(switch_id, switch_name, buttons)
    print("Renamed switch:", switch_id, switch_name, buttons)  # Debug log
    print("Current switches:", connection_manager.get_switches())  # Debug log
    connection_manager.save_to_db()

    return {"status": "Switch renamed"}


@router.post("/add_switch")
def add_switch(
    request: Request,
    switch_id: int = Form(...),
    switch_name: str = Form(...),
    buttons: int = Form(...),
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    connection_manager.add_switch(switch_id, switch_name, buttons)
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
    return JSONResponse(content=connections)


@router.get("/get_relays")
def get_relays(
    request: Request,
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    relays = connection_manager.get_relays()
    return JSONResponse(content=relays)


@router.get("/get_switches")
def get_switches(
    request: Request,
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    switches = connection_manager.get_switches()
    return JSONResponse(content=switches)


@router.get("/get_outputs")
def get_outputs(
    request: Request,
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    outputs = connection_manager.get_outputs()
    return JSONResponse(content=outputs)


@router.post("/remove_connection")
def remove_connection(
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

    connection_manager.remove_connection(switch_id, button_id, relay_id, output_id)
    connection_manager.save_to_db()
    return {"status": "Connection removed"}


@router.post("/remove_relay")
def remove_relay(
    request: Request,
    relay_id: int = Form(...),
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    connection_manager.remove_relay(relay_id)
    connection_manager.save_to_db()
    return {"status": "Relay removed"}


@router.post("/remove_switch")
def remove_switch(
    request: Request,
    switch_id: int = Form(...),
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    connection_manager.remove_switch(switch_id)
    connection_manager.save_to_db()
    return {"status": "Switch removed"}


connection_manager = ConnectionManager()
