"""Serialize the shared psycopg connection across HTTP, MQTT and WS workers."""

import asyncio
import inspect
from functools import wraps
from time import monotonic

from fastapi import HTTPException

from turbacz import validation


def serialized(method):
    signature = inspect.signature(method)
    registry_read = method.__name__ in {"get_relays", "get_switches"}
    registry_write = method.__name__ in {
        "add_relay", "rename_relay", "remove_relay",
        "add_switch", "rename_switch", "remove_switch",
    }

    @wraps(method)
    def call(self, *args, **kwargs):
        with self._db_lock:
            bound = signature.bind(self, *args, **kwargs)
            try:
                for key, value in bound.arguments.items():
                    if key in {"relay_id", "switch_id"}:
                        validation.device_id(value)
                    if key in {"output_id", "output_id_power", "output_id_direction"}:
                        validation.output_id(value)
                    if key == "button_id":
                        validation.button_id(value)
                    if key == "outputs":
                        validation.integer(value, 1, 16)
                    if key == "buttons":
                        validation.integer(value, 1, 24)
            except ValueError as exc:
                raise HTTPException(status_code=400, detail=str(exc)) from exc
            try:
                if registry_write:
                    self._registry_cache.clear()
                if registry_read:
                    cached = self._registry_cache.get(method.__name__)
                    if cached is not None and monotonic() - cached[0] < 60:
                        return cached[1].copy()
                result = method(self, *args, **kwargs)
                if registry_read:
                    # Values are immutable tuples; return a separate dict so
                    # callers cannot mutate the cached registry. The TTL also
                    # picks up changes made outside this process.
                    self._registry_cache[method.__name__] = (monotonic(), result.copy())
                return result
            except Exception:
                self.conn.rollback()
                raise

    return call


def serialized_manager(cls):
    for key, value in list(vars(cls).items()):
        if callable(value) and key not in {"__init__", "_init_db"}:
            setattr(cls, key, serialized(value))
    return cls


async def db_call(function, *args, **kwargs):
    return await asyncio.to_thread(function, *args, **kwargs)
