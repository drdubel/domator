"""One connection pool for telemetry writes and history queries."""

from contextlib import asynccontextmanager

import httpx

_client: httpx.AsyncClient | None = None


def get_metrics_client() -> httpx.AsyncClient:
    if _client is None:
        raise RuntimeError("Metrics HTTP client has not been started")
    return _client


@asynccontextmanager
async def metrics_client_lifespan():
    global _client
    # Keep TLS verification and normal timeout defaults. Reusing the client
    # avoids reloading the trust store and reconnecting for every mesh report.
    async with httpx.AsyncClient() as client:
        _client = client
        try:
            yield
        finally:
            _client = None
