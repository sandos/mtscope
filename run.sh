#!/usr/bin/with-contenv bashio
set -eu

TOPIC="$(bashio::config 'topic')"
RETENTION_DAYS="$(bashio::config 'retention_days')"
RESET_DATABASE="$(bashio::config 'reset_database')"
LOG_MQTT_EVENTS="$(bashio::config 'log_mqtt_events')"
if bashio::var.true "$RESET_DATABASE"; then
	bashio::log.warning "Removing stored monitor data"
	rm -f /data/meshat-monitor.db /data/meshat-monitor.db-shm /data/meshat-monitor.db-wal
fi
if bashio::var.true "$LOG_MQTT_EVENTS"; then
	set -- --log-mqtt-events
else
	set --
fi
bashio::log.info "Starting Meshtastic Monitor for topic ${TOPIC} on ingress port 8099"
exec meshat-monitor --database /data/meshat-monitor.db --topic "$TOPIC" --retention-days "$RETENTION_DAYS" --http-port 8099 --web-root /usr/local/share/meshat-monitor "$@"