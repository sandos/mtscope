#!/usr/bin/with-contenv bashio
set -eu

TOPIC="$(bashio::config 'topic')"
RETENTION_DAYS="$(bashio::config 'retention_days')"
HTTP_PORT="$(bashio::config 'http_port')"
exec meshat-monitor --database /data/meshat-monitor.db --topic "$TOPIC" --retention-days "$RETENTION_DAYS" --http-port "${HTTP_PORT:-8099}" --web-root /usr/local/share/meshat-monitor