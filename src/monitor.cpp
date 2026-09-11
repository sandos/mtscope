#include "monitor.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

#include "app.h"
#include "database.h"
#include "logger.h"
#include <mosquitto.h>

Monitor::Monitor(const Config& config, Database& database, Logger& logger) : config_(config), database_(database), logger_(logger) {}

Monitor::~Monitor() {
    if (client_) {
        mosquitto_loop_stop(client_, true);
        mosquitto_destroy(client_);
    }
    mosquitto_lib_cleanup();
}

void Monitor::run() {
    mosquitto_lib_init();
    const std::string client_id = "meshat-monitor-" + std::to_string(static_cast<long long>(getpid()));
    client_ = mosquitto_new(client_id.c_str(), true, this);
    if (!client_) throw std::runtime_error("Unable to create MQTT client");
    mosquitto_username_pw_set(client_, config_.username.c_str(), config_.password.c_str());
    mosquitto_connect_callback_set(client_, on_connect);
    mosquitto_disconnect_callback_set(client_, on_disconnect);
    mosquitto_message_callback_set(client_, on_message);
    mosquitto_reconnect_delay_set(client_, 1, 60, true);
    logger_.info("MQTT", "Starting client " + client_id);
    while (running) {
        const int result = mosquitto_connect_async(client_, config_.host.c_str(), config_.mqtt_port, 60);
        if (result == MOSQ_ERR_SUCCESS) break;
        std::cerr << "Unable to start MQTT connection: " << result << " (" << mosquitto_strerror(result) << "); retrying\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!running) return;
    mosquitto_loop_start(client_);
}

void Monitor::on_connect(mosquitto* client, void* context, int result) {
    auto* monitor = static_cast<Monitor*>(context);
    if (result != MOSQ_ERR_SUCCESS) {
        monitor->connected_.store(false);
        monitor->logger_.info("MQTT", "Connection failed (" + std::to_string(result) + "): " + std::string(mosquitto_strerror(result)));
        std::cerr << "MQTT connection failed: " << result << " (" << mosquitto_strerror(result) << ")\n";
        return;
    }
    const int subscribe_result = mosquitto_subscribe(client, nullptr, monitor->config_.topic.c_str(), 0);
    if (subscribe_result != MOSQ_ERR_SUCCESS) {
        monitor->connected_.store(false);
        std::cerr << "MQTT subscription failed: " << subscribe_result << " (" << mosquitto_strerror(subscribe_result) << ")\n";
        return;
    }
    monitor->connected_.store(true);
    monitor->logger_.info("MQTT", "Connected; subscribed to " + monitor->config_.topic);
    if (monitor->config_.log_mqtt_events) std::cerr << "MQTT connected; subscribed to " << monitor->config_.topic << "\n";
}

void Monitor::on_disconnect(mosquitto*, void* context, int result) {
    auto* monitor = static_cast<Monitor*>(context);
    monitor->connected_.store(false);
    monitor->logger_.info("MQTT", result == MOSQ_ERR_SUCCESS ? "Disconnected (0)" : "Disconnected (" + std::to_string(result) + "): " + std::string(mosquitto_strerror(result)));
    if (monitor->config_.log_mqtt_events && result != MOSQ_ERR_SUCCESS) {
        std::cerr << "MQTT disconnected: " << result << " (" << mosquitto_strerror(result) << ")\n";
    } else if (monitor->config_.log_mqtt_events) {
        std::cerr << "MQTT disconnected\n";
    }
}

void Monitor::on_message(mosquitto*, void* context, const mosquitto_message* message) {
    auto* monitor = static_cast<Monitor*>(context);
    if (message->payload && message->payloadlen > 0) {
        if (!monitor->database_.insert(message->topic, message->payload, message->payloadlen)) {
            std::cerr << "Unable to persist MQTT message from " << message->topic << "\n";
        }
    }
}