#include "monitor.h"

#include <iostream>
#include <stdexcept>

#include "database.h"
#include <mosquitto.h>

Monitor::Monitor(const Config& config, Database& database) : config_(config), database_(database) {}

Monitor::~Monitor() {
    if (client_) {
        mosquitto_loop_stop(client_, true);
        mosquitto_destroy(client_);
    }
    mosquitto_lib_cleanup();
}

void Monitor::run() {
    mosquitto_lib_init();
    client_ = mosquitto_new("meshat-monitor", true, this);
    if (!client_) throw std::runtime_error("Unable to create MQTT client");
    mosquitto_username_pw_set(client_, config_.username.c_str(), config_.password.c_str());
    mosquitto_connect_callback_set(client_, on_connect);
    mosquitto_disconnect_callback_set(client_, on_disconnect);
    mosquitto_message_callback_set(client_, on_message);
    mosquitto_reconnect_delay_set(client_, 1, 60, true);
    if (mosquitto_connect_async(client_, config_.host.c_str(), config_.mqtt_port, 60) != MOSQ_ERR_SUCCESS) {
        throw std::runtime_error("Unable to connect MQTT client");
    }
    mosquitto_loop_start(client_);
}

void Monitor::on_connect(mosquitto* client, void* context, int result) {
    auto* monitor = static_cast<Monitor*>(context);
    if (result != MOSQ_ERR_SUCCESS) {
        monitor->connected_.store(false);
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
    std::cerr << "Subscribed to " << monitor->config_.topic << "\n";
}

void Monitor::on_disconnect(mosquitto*, void* context, int result) {
    auto* monitor = static_cast<Monitor*>(context);
    monitor->connected_.store(false);
    if (result != MOSQ_ERR_SUCCESS) {
        std::cerr << "MQTT disconnected: " << result << " (" << mosquitto_strerror(result) << ")\n";
    } else {
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