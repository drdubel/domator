FROM ghcr.io/astral-sh/uv:0.12.17 AS uv

# Pin the Debian base variant instead of following the moving `slim` alias.
# This also avoids reusing a corrupted Trixie layer seen on ARM installations.
FROM python:3.14-slim-bookworm

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    UV_LINK_MODE=copy \
    UV_PYTHON_DOWNLOADS=0

WORKDIR /app

# Copy the pinned, static uv binary from its official image. Installing uv with
# pip created an extra writable overlay layer which proved unreliable on the
# ARM/SD-card deployment.
COPY --from=uv /uv /uvx /bin/

COPY pyproject.toml uv.lock ./

COPY static ./static
COPY turbacz ./turbacz
COPY docker-entrypoint.sh ./docker-entrypoint.sh

RUN chmod +x /app/docker-entrypoint.sh \
    && uv sync --frozen --no-dev \
    && uv pip install --python /app/.venv/bin/python "psycopg[binary]==3.3.2"

EXPOSE 8000

CMD ["/app/docker-entrypoint.sh"]
