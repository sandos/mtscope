#!/usr/bin/with-contenv bashio
set -eu

TOPIC="$(bashio::config 'topic')"
RETENTION_DAYS="$(bashio::config 'retention_days')"
RESET_DATABASE="$(bashio::config 'reset_database')"
if bashio::var.true "$RESET_DATABASE"; then
	bashio::log.warning "Removing stored monitor data"
	rm -f /data/meshat-monitor.db /data/meshat-monitor.db-shm /data/meshat-monitor.db-wal
fi
exec meshat-monitor --database /data/meshat-monitor.db --topic "$TOPIC" --retention-days "$RETENTION_DAYS" --http-port 8099 --web-root /usr/local/share/meshat-monitor