"""Serialize the shared psycopg connection across HTTP, MQTT and WS workers."""

import asyncio
import inspect
from functools import wraps

from fastapi import HTTPException

from turbacz import validation


def serialized(method):
    signature = inspect.signature(method)

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
                return method(self, *args, **kwargs)
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
