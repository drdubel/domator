"""Bound request streams before parsers allocate memory or temporary files."""

import asyncio

from starlette.middleware.body_limit import RequestBodyLimitMiddleware
from starlette.responses import JSONResponse


class CustomRequestSizeMiddleware:
    def __init__(self, app, max_content_size):
        self.app = RequestBodyLimitMiddleware(app, max_body_size=max_content_size)

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            return await self.app(scope, receive, send)
        lengths = [v for k, v in scope["headers"] if k.lower() == b"content-length"]
        if lengths and (len(lengths) != 1 or not lengths[0].isdigit()):
            return await JSONResponse({"detail": "Invalid Content-Length"}, 400)(
                scope, receive, send
            )

        async def bounded_receive():
            # An authenticated upload cannot monopolize its concurrency slot
            # indefinitely by stopping midway through the request body.
            try:
                return await asyncio.wait_for(receive(), timeout=30)
            except TimeoutError:
                from starlette.exceptions import HTTPException

                raise HTTPException(408, "Request body timeout") from None

        await self.app(scope, bounded_receive, send)
