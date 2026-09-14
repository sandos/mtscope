#!/usr/bin/env bash
set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${MTSCOPE_ASAN:-0}" == "1" ]]; then
    BINARY="${MTSCOPE_BINARY:-$SCRIPT_DIR/build/asan/meshat-monitor}"
    BUILD_COMMAND='cmake --preset asan && cmake --build --preset asan --target meshat-monitor'
else
    BINARY="${MTSCOPE_BINARY:-$SCRIPT_DIR/build/release/meshat-monitor}"
    BUILD_COMMAND='cmake --preset release && cmake --build --preset release'
fi
DATABASE="${MTSCOPE_DATABASE:-$SCRIPT_DIR/meshat-monitor.db}"
TOPIC="${MTSCOPE_TOPIC:-msh/#}"
RETENTION_DAYS="${MTSCOPE_RETENTION_DAYS:-3}"
HTTP_PORT="${MTSCOPE_HTTP_PORT:-8099}"
WEB_ROOT="${MTSCOPE_WEB_ROOT:-$SCRIPT_DIR/web}"

if [[ ! -x "$BINARY" ]]; then
    printf 'Monitor binary not found or not executable: %s\n' "$BINARY" >&2
    printf 'Build it first with: %s\n' "$BUILD_COMMAND" >&2
    exit 1
fi

exec "$BINARY" \
    --database "$DATABASE" \
    --topic "$TOPIC" \
    --retention-days "$RETENTION_DAYS" \
    --http-port "$HTTP_PORT" \
    --web-root "$WEB_ROOT" \
    "$@"
