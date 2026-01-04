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
        self._outputs: dict[int, dict[str, tuple[str, int]]] = {}
        self._sections: dict[int, str] = {}
        self._removed: bool = False

        self._init_db()
        self.create_tables()
        self.load_from_db()

        if not self._sections:
            self.add_section("Default")

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
            CREATE TABLE IF NOT EXISTS sections (
                id INTEGER PRIMARY KEY,
                name TEXT NOT NULL
            );
            """
        )
        self.cur.execute(
            """
            CREATE TABLE IF NOT EXISTS outputs (
                relay_id BIGINT REFERENCES relays(id),
                output_id TEXT NOT NULL,
                name TEXT NOT NULL,
                section_id INTEGER,
                PRIMARY KEY (relay_id, output_id)
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
                output_id TEXT,
                PRIMARY KEY (switch_id, button_id, relay_id, output_id)
            );
            """
        )
        self.conn.commit()

    def load_from_db(self):
        self.cur.execute("SELECT id, name FROM relays;")
        for relay_id, relay_name in self.cur.fetchall():
            self._relays[relay_id] = relay_name

        self.cur.execute("SELECT id, name, buttons FROM switches;")
        for switch_id, switch_name, buttons in self.cur.fetchall():
            self._switches[switch_id] = (switch_name, buttons)

        self.cur.execute("SELECT relay_id, output_id, section_id, name FROM outputs;")
        for relay_id, output_id, section_id, output_name in self.cur.fetchall():
            if relay_id not in self._outputs:
                self._outputs[relay_id] = {}
            self._outputs[relay_id][output_id] = (output_name, section_id)

        self.cur.execute(
            "SELECT switch_id, button_id, relay_id, output_id FROM connections;"
        )
        for switch_id, button_id, relay_id, output_id in self.cur.fetchall():
            if switch_id not in self._connections:
                self._connections[switch_id] = {}

            if button_id not in self._connections[switch_id]:
                self._connections[switch_id][button_id] = []

            self._connections[switch_id][button_id].append((relay_id, output_id))

        self.cur.execute("SELECT id, name FROM sections;")
        for section_id, section_name in self.cur.fetchall():
            if section_id not in self._sections:
                self._sections[section_id] = []
            self._sections[section_id].append(section_name)

    def add_relay(self, relay_id: int, relay_name: str):
        self._relays[relay_id] = relay_name

        self.cur.execute(
            """
            INSERT INTO relays (id, name)
            VALUES (%s, %s)
            ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name;
            """,
            (relay_id, relay_name),
        )

        for i in range(8):
            self.add_output(relay_id, chr(97 + i), f"Output {i + 1}", 0)

        self.conn.commit()

    def get_relays(self) -> dict[int, str] | None:
        return self._relays

    def rename_relay(self, relay_id: int, relay_name: str):
        if relay_id in self._relays:
            self._relays[relay_id] = relay_name

        self.cur.execute(
            """
            UPDATE relays
            SET name = %s
            WHERE id = %s;
            """,
            (relay_name, relay_id),
        )
        self.conn.commit()

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

        self.cur.execute(
            """
            DELETE FROM relays
            WHERE id = %s;
            """,
            (relay_id,),
        )

        self.cur.execute(
            """
            DELETE FROM outputs
            WHERE relay_id = %s;
            """,
            (relay_id,),
        )

        self.cur.execute(
            """
            DELETE FROM connections
            WHERE relay_id = %s;
            """,
            (relay_id,),
        )

        self.conn.commit()

    def add_switch(self, switch_id: int, switch_name: str, buttons: int = 3):
        self._switches[switch_id] = (switch_name, buttons)

        self.cur.execute(
            """
            INSERT INTO switches (id, name, buttons)
            VALUES (%s, %s, %s)
            ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name, buttons = EXCLUDED.buttons;
            """,
            (switch_id, switch_name, buttons),
        )
        self.conn.commit()

    def get_switches(self) -> dict[int, tuple[str, int]] | None:
        return self._switches

    def rename_switch(self, switch_id: int, switch_name: str, buttons: int):
        if switch_id in self._switches:
            self._switches[switch_id] = (
                switch_name,
                buttons,
            )

            self.cur.execute(
                """
                UPDATE switches
                SET name = %s, buttons = %s
                WHERE id = %s;
                """,
                (switch_name, buttons, switch_id),
            )
            self.conn.commit()

    def remove_switch(self, switch_id: int):
        if switch_id in self._connections:
            del self._connections[switch_id]
            self.cur.execute(
                """
                DELETE FROM connections
                WHERE switch_id = %s;
                """,
                (switch_id,),
            )
            self.conn.commit()

        if switch_id in self._switches:
            del self._switches[switch_id]
            self.cur.execute(
                """
                DELETE FROM switches
                WHERE id = %s;
                """,
                (switch_id,),
            )
            self.conn.commit()

    def add_output(
        self, relay_id: int, output_id: str, output_name: str, section_id: int = 0
    ):
        self._outputs.setdefault(relay_id, {})[output_id] = (output_name, section_id)

        self.cur.execute(
            """
            INSERT INTO outputs (relay_id, output_id, name, section_id)
            VALUES (%s, %s, %s, %s)
            ON CONFLICT (relay_id, output_id) DO NOTHING;
            """,
            (relay_id, output_id, output_name, section_id),
        )

    def name_output(self, relay_id: int, output_id: str, output_name: str):
        section_id = self._outputs.get(relay_id, {}).get(output_id, ("", 0))[1]
        self._outputs.setdefault(relay_id, {})[output_id] = (output_name, section_id)

        self.cur.execute(
            """
            UPDATE outputs
            SET name = %s
            WHERE relay_id = %s AND output_id = %s;
            """,
            (output_name, relay_id, output_id),
        )
        self.conn.commit()

    def change_output_section(self, relay_id: int, output_id: str, section_id: int):
        if relay_id in self._outputs and output_id in self._outputs[relay_id]:
            output_name, _ = self._outputs[relay_id][output_id]
            self._outputs[relay_id][output_id] = (output_name, section_id)

            self.cur.execute(
                """
                UPDATE outputs
                SET section_id = %s
                WHERE relay_id = %s AND output_id = %s;
                """,
                (section_id, relay_id, output_id),
            )
            self.conn.commit()

    def get_outputs(self) -> dict[int, dict[str, tuple[str, int]]] | None:
        return self._outputs

    def get_named_outputs(self) -> dict[int, dict[str, tuple[str, int]]] | None:
        named_outputs = {}

        for relay_id, outputs in self._outputs.items():
            named_outputs[relay_id] = {
                output_id: (output[0], output[1])
                for output_id, output in outputs.items()
                if not output[0].startswith("Output ")
            }

        return named_outputs

    def add_section(self, section_name: str):
        if section_name not in self._sections.values():
            self._sections[len(self._sections)] = section_name

            self.cur.execute(
                """
                INSERT INTO sections (id, name)
                VALUES (%s, %s)
                ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name;
                """,
                (len(self._sections) - 1, section_name),
            )
            self.conn.commit()

    def remove_section(self, section_name: str):
        if section_name in self._sections.values():
            section_id = [
                key for key, value in self._sections.items() if value == section_name
            ][0]
            del self._sections[section_id]

            self.cur.execute(
                """
                DELETE FROM sections
                WHERE id = %s;
                """,
                (section_id,),
            )

            for relay_id in self._outputs:
                for output_id in self._outputs[relay_id]:
                    output_name, output_section_id = self._outputs[relay_id][output_id]
                    if output_section_id == section_id:
                        self._outputs[relay_id][output_id] = (output_name, 0)
                        self.cur.execute(
                            """
                            UPDATE outputs
                            SET section_id = %s
                            WHERE relay_id = %s AND output_id = %s;
                            """,
                            (0, relay_id, output_id),
                        )
            self.conn.commit()

    def get_sections(self) -> dict[int, str] | None:
        return self._sections

    def add_connection(
        self, switch_id: int, button_id: str, relay_id: int, output_id: str
    ):
        if switch_id not in self._connections:
            self._connections[switch_id] = {}

        if button_id not in self._connections[switch_id]:
            self._connections[switch_id][button_id] = []

        self._connections[switch_id][button_id].append((relay_id, output_id))

        self.cur.execute(
            """
            INSERT INTO connections (switch_id, button_id, relay_id, output_id)
            VALUES (%s, %s, %s, %s)
            ON CONFLICT (switch_id, button_id, relay_id, output_id) DO NOTHING;
            """,
            (switch_id, button_id, relay_id, output_id),
        )
        self.conn.commit()

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

            self.cur.execute(
                """
                DELETE FROM connections
                WHERE switch_id = %s AND button_id = %s AND relay_id = %s AND output_id = %s;
                """,
                (switch_id, button_id, relay_id, output_id),
            )
            self.conn.commit()

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
    return {"status": "Switch removed"}


connection_manager = ConnectionManager()
