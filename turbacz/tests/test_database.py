import os
from threading import RLock
from unittest.mock import MagicMock
from uuid import uuid4

import psycopg
import pytest
from psycopg import sql
from psycopg.pq import TransactionStatus

from turbacz.database import serialized_manager


@serialized_manager
class Manager:
    def __init__(self):
        self._db_lock = RLock()
        self._registry_cache = {}
        self.conn = MagicMock()

    def read(self):
        return self.conn.execute("SELECT 1").fetchone()

    def nested_read(self):
        result = self.read()
        self.conn.commit.assert_not_called()
        return result

    def fail(self, exception):
        self.conn.execute("SELECT 1")
        raise exception

    def get_relays(self):
        return dict(self.conn.execute("SELECT id, name FROM relays").fetchall())


def test_read_commits_before_returning():
    manager = Manager()
    manager.conn.execute.return_value.fetchone.return_value = (1,)
    assert manager.read() == (1,)
    manager.conn.commit.assert_called_once()


def test_nested_read_does_not_commit_the_callers_transaction():
    manager = Manager()
    manager.nested_read()
    manager.conn.commit.assert_called_once()
    assert manager._db_call_depth == 0


@pytest.mark.parametrize("exception", [ValueError("failed"), KeyboardInterrupt()])
def test_failed_or_interrupted_call_rolls_back(exception):
    manager = Manager()
    with pytest.raises(type(exception)):
        manager.fail(exception)
    manager.conn.rollback.assert_called_once()
    manager.conn.commit.assert_not_called()
    assert manager._db_call_depth == 0


def test_cached_registry_read_does_not_open_another_transaction():
    manager = Manager()
    manager.conn.execute.return_value.fetchall.return_value = [(1, "Relay")]
    assert manager.get_relays() == {1: "Relay"}
    assert manager.get_relays() == {1: "Relay"}
    manager.conn.execute.assert_called_once()
    manager.conn.commit.assert_called_once()


def test_second_instance_can_initialize_after_first_instance_reads():
    """Opt in with a disposable PostgreSQL DSN; only a unique schema is used."""
    dsn = os.environ.get("TURBACZ_TEST_POSTGRES_DSN")
    if not dsn:
        pytest.skip("set TURBACZ_TEST_POSTGRES_DSN to run PostgreSQL regression")

    # Import without connecting the module-level singleton to a configured DB.
    from unittest.mock import patch

    connection = MagicMock()
    cursor = connection.cursor.return_value.__enter__.return_value
    cursor.execute.return_value = cursor
    cursor.fetchone.return_value = [1]
    cursor.fetchall.return_value = []
    with patch("psycopg.connect", return_value=connection):
        from turbacz.connection_manager import ConnectionManager

    schema = sql.Identifier("turbacz_test_" + uuid4().hex)
    with psycopg.connect(dsn, autocommit=True) as admin:
        admin.execute(sql.SQL("CREATE SCHEMA {}").format(schema))
        try:
            with psycopg.connect(dsn) as first, psycopg.connect(dsn) as second:
                managers = []
                for conn in (first, second):
                    conn.execute(sql.SQL("SET search_path TO {}").format(schema))
                    conn.execute("SET statement_timeout = '2s'")
                    conn.commit()
                    manager = ConnectionManager.__new__(ConnectionManager)
                    manager._db_lock = RLock()
                    manager._registry_cache = {}
                    manager.conn = conn
                    managers.append(manager)

                one, two = managers
                one.create_tables()
                one.add_section("Default")
                one.add_relay(111, "Relay")
                # Also exercise the initializer's early-return branch once
                # output positions have already been populated.
                one.create_tables()
                for read in (one.get_sections, one.get_outputs, one.get_relays):
                    read()
                    assert first.info.transaction_status == TransactionStatus.IDLE
                two.create_tables()
                assert two.get_relays() == {111: ("Relay", 8)}
                assert second.info.transaction_status == TransactionStatus.IDLE
                from turbacz.zigbee_knob import KNOB_BUTTONS, KNOB_SWITCH_ID

                with patch.object(ConnectionManager, "_init_db", lambda self: setattr(self, "conn", first)):
                    initialized = ConnectionManager()
                assert initialized.get_switches()[KNOB_SWITCH_ID] == ("tyua_knob", len(KNOB_BUTTONS))
                assert one.get_zigbee_knob_config()["buttons"]["d"]["command"] == "off"
                one.set_zigbee_knob_command("d", "up")
                one.rename_switch(KNOB_SWITCH_ID, "Living room knob", len(KNOB_BUTTONS))
                with patch.object(ConnectionManager, "_init_db", lambda self: setattr(self, "conn", second)):
                    restarted = ConnectionManager()
                assert restarted.get_switches()[KNOB_SWITCH_ID] == ("Living room knob", len(KNOB_BUTTONS))
                assert restarted.get_zigbee_knob_config()["buttons"]["d"]["command"] == "up"
                assert second.info.transaction_status == TransactionStatus.IDLE
        finally:
            admin.execute(sql.SQL("DROP SCHEMA {} CASCADE").format(schema))
