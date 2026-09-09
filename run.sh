#!/usr/bin/with-contenv bashio
set -eu

TOPIC="$(bashio::config 'topic')"
RETENTION_DAYS="$(bashio::config 'retention_days')"
exec meshat-monitor --database /data/meshat-monitor.db --topic "$TOPIC" --retention-days "$RETENTION_DAYS" --http-port 8099 --web-root /usr/local/share/meshat-monitor