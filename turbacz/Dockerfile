FROM python:3.14-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app

RUN pip install --no-cache-dir uv

COPY pyproject.toml uv.lock ./

COPY static ./static
COPY turbacz ./turbacz
COPY docker-entrypoint.sh ./docker-entrypoint.sh
RUN uv sync --frozen --no-dev
RUN uv pip install --python /app/.venv/bin/python "psycopg[binary]==3.3.2"

RUN chmod +x /app/docker-entrypoint.sh

EXPOSE 8000

CMD ["/app/docker-entrypoint.sh"]
