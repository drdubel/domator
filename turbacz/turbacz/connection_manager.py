from typing import Optional

import psycopg
from fastapi import APIRouter, Cookie, Form, Request
from fastapi.responses import JSONResponse

import turbacz.auth as auth
from turbacz.settings import config

connection_router = APIRouter(prefix="/lights")


class ConnectionManager:
    def __init__(self):
        self.rootId: Optional[int] = None

        self._init_db()
        self.create_tables()

        if not self.get_sections():
            self.add_section("Default")

    def _init_db(self):
        self.conn = psycopg.connect(
            f"dbname={config.psql.dbname} user={config.psql.user} password={config.psql.password} host={config.psql.host} port={config.psql.port}"
        )

    def create_tables(self):
        with self.conn.cursor() as cur:
            cur.execute(
                """
                CREATE TABLE IF NOT EXISTS relays (
                    id BIGINT PRIMARY KEY,
                    name TEXT NOT NULL,
                    outputs INTEGER NOT NULL DEFAULT 8
                );
                """
            )
            cur.execute(
                """
                CREATE TABLE IF NOT EXISTS sections (
                    id SERIAL PRIMARY KEY,
                    name TEXT NOT NULL
                );
                """
            )
            cur.execute(
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
            cur.execute(
                """
                CREATE TABLE IF NOT EXISTS switches (
                    id BIGINT PRIMARY KEY,
                    name TEXT NOT NULL,
                    buttons INTEGER NOT NULL
                );
                """
            )
            cur.execute(
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
            cur.execute(
                """
                CREATE TABLE IF NOT EXISTS buttons (
                    switch_id BIGINT REFERENCES switches(id),
                    button_id TEXT,
                    type INT
                );
                """
            )

        self.conn.commit()

    def add_relay(self, relay_id: int, relay_name: str, outputs: int = 8):
        if outputs not in (8, 16):
            raise ValueError("Outputs must be either 8 or 16")

        with self.conn.cursor() as cur:
            cur.execute(
                """
                INSERT INTO relays (id, name, outputs)
                VALUES (%s, %s, %s)
                ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name, outputs = EXCLUDED.outputs;
                """,
                (relay_id, relay_name, outputs),
            )

        for i in range(outputs):
            output_name = f"Output {i + 1}"
            self.add_output(relay_id, str(chr(97 + i)), output_name)

        self.conn.commit()

    def get_relays(self) -> dict[int, tuple[str, int]]:
        with self.conn.cursor() as cur:
            cur.execute("SELECT id, name, outputs FROM relays;")
            relays = cur.fetchall()

        return {row[0]: (row[1], row[2]) for row in relays}

    def rename_relay(self, relay_id: int, relay_name: str, outputs: int):
        if outputs not in (8, 16):
            raise ValueError("Outputs must be either 8 or 16")

        with self.conn.cursor() as cur:
            cur.execute(
                """
                UPDATE relays
                SET name = %s, outputs = %s
                WHERE id = %s;
                """,
                (relay_name, outputs, relay_id),
            )

        self.conn.commit()

    def remove_relay(self, relay_id: int):
        with self.conn.cursor() as cur:
            cur.execute(
                """
                DELETE FROM outputs
                WHERE relay_id = %s;
                """,
                (relay_id,),
            )
            cur.execute(
                """
                DELETE FROM connections
                WHERE relay_id = %s;
                """,
                (relay_id,),
            )
            cur.execute(
                """
                DELETE FROM relays
                WHERE id = %s;
                """,
                (relay_id,),
            )

        self.conn.commit()

    def add_switch(self, switch_id: int, switch_name: str, buttons: int = 3):
        with self.conn.cursor() as cur:
            cur.execute(
                """
                INSERT INTO switches (id, name, buttons)
                VALUES (%s, %s, %s)
                ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name, buttons = EXCLUDED.buttons;
                """,
                (switch_id, switch_name, buttons),
            )

        self.conn.commit()

        for i in range(buttons):
            button_id = chr(97 + i)
            self.add_button(switch_id, button_id)

    def get_switches(self) -> dict[int, tuple[str, int]]:
        with self.conn.cursor() as cur:
            cur.execute("SELECT id, name, buttons FROM switches;")
            switches = cur.fetchall()

        return {row[0]: (row[1], row[2]) for row in switches}

    def rename_switch(self, switch_id: int, switch_name: str, buttons: int):
        with self.conn.cursor() as cur:
            for i in range(buttons, self.get_switches()[switch_id][1]):
                button_id = chr(97 + i)
                cur.execute(
                    """
                    DELETE FROM buttons
                    WHERE switch_id = %s AND button_id = %s;
                    """,
                    (switch_id, button_id),
                )

            for i in range(self.get_switches()[switch_id][1], buttons):
                button_id = chr(97 + i)
                cur.execute(
                    """
                    INSERT INTO buttons (switch_id, button_id, type)
                    VALUES (%s, %s, %s)
                    ON CONFLICT (switch_id, button_id) DO NOTHING;
                    """,
                    (switch_id, button_id, 0),
                )

            cur.execute(
                """
                UPDATE switches
                SET name = %s, buttons = %s
                WHERE id = %s;
                """,
                (switch_name, buttons, switch_id),
            )

        self.conn.commit()

    def remove_switch(self, switch_id: int):
        with self.conn.cursor() as cur:
            cur.execute(
                """
                DELETE FROM connections
                WHERE switch_id = %s;
                """,
                (switch_id,),
            )

            cur.execute(
                """
                DELETE FROM buttons
                WHERE switch_id = %s;
                """,
                (switch_id,),
            )

            cur.execute(
                """
                DELETE FROM switches
                WHERE id = %s;
                """,
                (switch_id,),
            )

        self.conn.commit()

    def add_output(self, relay_id: int, output_id: str, output_name: str, section_id: int = 0):
        with self.conn.cursor() as cur:
            cur.execute(
                """
                INSERT INTO outputs (relay_id, output_id, name, section_id)
                VALUES (%s, %s, %s, %s)
                ON CONFLICT (relay_id, output_id) DO NOTHING;
                """,
                (relay_id, output_id, output_name, section_id),
            )

        self.conn.commit()

    def name_output(self, relay_id: int, output_id: str, output_name: str):
        with self.conn.cursor() as cur:
            cur.execute(
                """
                UPDATE outputs
                SET name = %s
                WHERE relay_id = %s AND output_id = %s;
                """,
                (output_name, relay_id, output_id),
            )

        self.conn.commit()

    def change_output_section(self, relay_id: int, output_id: str, section_id: int):
        with self.conn.cursor() as cur:
            cur.execute(
                """
                UPDATE outputs
                SET section_id = %s
                WHERE relay_id = %s AND output_id = %s;
                """,
                (section_id, relay_id, output_id),
            )

        self.conn.commit()

    def get_outputs(self) -> dict[int, dict[str, tuple[str, int]]]:
        with self.conn.cursor() as cur:
            cur.execute("SELECT relay_id, output_id, name, section_id FROM outputs;")
            outputs = cur.fetchall()

        output_dict = {}
        for row in outputs:
            relay_id, output_id, name, section_id = row
            if relay_id not in output_dict:
                output_dict[relay_id] = {}
            output_dict[relay_id][output_id] = (name, section_id)

        return output_dict

    def get_named_outputs(self) -> dict[int, dict[str, tuple[str, int]]]:
        named_outputs = {}

        for relay_id, outputs in self.get_outputs().items():
            named_outputs[relay_id] = {
                output_id: (output[0], output[1])
                for output_id, output in outputs.items()
                if not output[0].startswith("Output ")
            }

        return named_outputs

    def add_section(self, section_name: str):
        with self.conn.cursor() as cur:
            cur.execute(
                """
                INSERT INTO sections (name)
                VALUES (%s)
                ON CONFLICT (id) DO NOTHING;
                """,
                (section_name,),
            )

        self.conn.commit()

    def remove_section(self, section_name: str):
        with self.conn.cursor() as cur:
            cur.execute(
                """
                SELECT id FROM sections
                WHERE name = %s;
                """,
                (section_name,),
            )
            section = cur.fetchone()
            if not section:
                return

            section_id = section[0]

            cur.execute(
                """
                    DELETE FROM sections
                    WHERE id = %s;
                    """,
                (section_id,),
            )
            cur.execute(
                """
                    UPDATE outputs
                    SET section_id = 0
                    WHERE section_id = %s;
                    """,
                (section_id,),
            )

        self.conn.commit()

    def get_sections(self) -> dict[int, str] | None:
        with self.conn.cursor() as cur:
            cur.execute("SELECT id, name FROM sections;")
            sections = cur.fetchall()

        return {row[0]: row[1] for row in sections}

    def add_button(self, switch_id: int, button_id: str, button_type: int = 0):
        with self.conn.cursor() as cur:
            cur.execute(
                """
                INSERT INTO buttons (switch_id, button_id, type)
                VALUES (%s, %s, %s)
                ON CONFLICT (switch_id, button_id) DO UPDATE SET type = EXCLUDED.type;
                """,
                (switch_id, button_id, button_type),
            )

        self.conn.commit()

    def get_buttons(self, switch_id: int) -> dict[str, int]:
        with self.conn.cursor() as cur:
            cur.execute(
                """
                SELECT button_id, type FROM buttons
                WHERE switch_id = %s;
                """,
                (switch_id,),
            )
            buttons = cur.fetchall()

        return {row[0]: row[1] for row in buttons}

    def get_all_buttons(self) -> dict[int, dict[str, int]]:
        with self.conn.cursor() as cur:
            cur.execute("SELECT switch_id, button_id, type FROM buttons;")
            buttons = cur.fetchall()

        button_dict = {}
        for row in buttons:
            switch_id, button_id, button_type = row
            if switch_id not in button_dict:
                button_dict[switch_id] = {}

            button_dict[switch_id][button_id] = button_type

        return button_dict

    def set_button_type(self, switch_id: int, button_id: str, button_type: int):
        with self.conn.cursor() as cur:
            exists = cur.execute(
                """
                SELECT 1 FROM buttons
                WHERE switch_id = %s AND button_id = %s;
                """,
                (switch_id, button_id),
            ).fetchone()

            if not exists:
                cur.execute(
                    """
                    INSERT INTO buttons (switch_id, button_id, type)
                    VALUES (%s, %s, %s);
                    """,
                    (switch_id, button_id, button_type),
                )
            else:
                cur.execute(
                    """
                    UPDATE buttons
                    SET type = %s
                    WHERE switch_id = %s AND button_id = %s;
                    """,
                    (button_type, switch_id, button_id),
                )

        self.conn.commit()

    def remove_button(self, switch_id: int, button_id: str):
        with self.conn.cursor() as cur:
            cur.execute(
                """
                DELETE FROM buttons
                WHERE switch_id = %s AND button_id = %s;
                """,
                (switch_id, button_id),
            )

        self.conn.commit()

    def add_connection(self, switch_id: int, button_id: str, relay_id: int, output_id: str):
        with self.conn.cursor() as cur:
            cur.execute(
                """
                INSERT INTO connections (switch_id, button_id, relay_id, output_id)
                VALUES (%s, %s, %s, %s)
                ON CONFLICT (switch_id, button_id, relay_id, output_id) DO NOTHING;
                """,
                (switch_id, button_id, relay_id, output_id),
            )

        self.conn.commit()

    def remove_connection(self, switch_id: int, button_id: str, relay_id: int, output_id: str):
        with self.conn.cursor() as cur:
            cur.execute(
                """
                DELETE FROM connections
                WHERE switch_id = %s AND button_id = %s AND relay_id = %s AND output_id = %s;
                """,
                (switch_id, button_id, relay_id, output_id),
            )

        self.conn.commit()

    def get_connection(self, switch_id: int, button_id: str) -> tuple[int, str] | None:
        with self.conn.cursor() as cur:
            cur.execute(
                """
                SELECT relay_id, output_id FROM connections
                WHERE switch_id = %s AND button_id = %s;
                """,
                (switch_id, button_id),
            )
            connection = cur.fetchone()

        return connection

    def get_all_connections(self) -> dict[int, dict[str, tuple[int, str]]]:
        with self.conn.cursor() as cur:
            cur.execute("SELECT switch_id, button_id, relay_id, output_id FROM connections;")
            connections = cur.fetchall()

        connection_dict = {}
        for row in connections:
            switch_id, button_id, relay_id, output_id = row
            if switch_id not in connection_dict:
                connection_dict[switch_id] = {}

            if button_id not in connection_dict[switch_id]:
                connection_dict[switch_id][button_id] = []

            connection_dict[switch_id][button_id].append((relay_id, output_id))

        return connection_dict


@connection_router.post("/add_relay")
def add_relay(
    request: Request,
    relay_id: int = Form(...),
    relay_name: str = Form(...),
    outputs: int = Form(8),
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    connection_manager.add_relay(relay_id, relay_name, outputs)

    return {"status": "Relay added"}


@connection_router.post("/name_output")
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


@connection_router.post("/rename_relay")
def rename_relay(
    request: Request,
    relay_id: int = Form(...),
    relay_name: str = Form(...),
    outputs: int = Form(...),
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    connection_manager.rename_relay(relay_id, relay_name, outputs)

    return {"status": "Relay renamed"}


@connection_router.post("/rename_switch")
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


@connection_router.post("/add_switch")
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


@connection_router.post("/add_connection")
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


@connection_router.get("/get_connections")
def get_connections(
    request: Request,
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    connections = connection_manager.get_all_connections()
    return JSONResponse(content=connections)


@connection_router.get("/get_relays")
def get_relays(
    request: Request,
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    relays = connection_manager.get_relays()
    return JSONResponse(content=relays)


@connection_router.get("/get_switches")
def get_switches(
    request: Request,
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    switches = connection_manager.get_switches()
    return JSONResponse(content=switches)


@connection_router.get("/get_outputs")
def get_outputs(
    request: Request,
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    outputs = connection_manager.get_outputs()
    return JSONResponse(content=outputs)


@connection_router.get("/get_all_buttons")
def get_all_buttons(
    request: Request,
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    buttons = connection_manager.get_all_buttons()
    return JSONResponse(content=buttons)


@connection_router.post("/remove_connection")
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


@connection_router.post("/remove_relay")
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


@connection_router.post("/remove_switch")
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


@connection_router.post("/remove_button")
def remove_button(
    request: Request,
    button_id: str = Form(...),
    access_token: Optional[str] = Cookie(None),
):
    user = auth.get_current_user(access_token)

    if not user:
        return {"error": "Unauthorized"}

    connection_manager.remove_button(button_id)
    return {"status": "Button removed"}


connection_manager = ConnectionManager()
