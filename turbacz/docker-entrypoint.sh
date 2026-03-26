#!/usr/bin/env sh
set -eu

if [ ! -f /app/turbacz.toml ]; then
  echo "Missing /app/turbacz.toml. Mount or copy a config file before starting."
  exit 1
fi

exec /app/.venv/bin/turbacz
