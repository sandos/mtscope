#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <mosquitto.h>

namespace {

struct Probe {
    const char* topic = nullptr;
    std::atomic<bool> connected{false};
    std::atomic<unsigned long> messages{0};
    std::atomic<unsigned long> disconnects{0};
};

void on_connect(mosquitto* client, void* context, int result) {
    auto* probe = static_cast<Probe*>(context);
    if (result != MOSQ_ERR_SUCCESS) {
        std::cerr << "connect failed: " << result << " (" << mosquitto_strerror(result) << ")\n";
        return;
    }
    const int subscribe_result = mosquitto_subscribe(client, nullptr, probe->topic, 0);
    std::cout << "connected; subscribe=" << subscribe_result << "\n";
    probe->connected.store(subscribe_result == MOSQ_ERR_SUCCESS);
}

void on_disconnect(mosquitto*, void* context, int result) {
    auto* probe = static_cast<Probe*>(context);
    probe->connected.store(false);
    ++probe->disconnects;
    std::cout << "disconnected: " << result << " (" << mosquitto_strerror(result) << ")\n";
}

void on_message(mosquitto*, void* context, const mosquitto_message*) {
    auto* probe = static_cast<Probe*>(context);
    ++probe->messages;
}

const char* value_or(const char* value, const char* fallback) {
    return value && *value ? value : fallback;
}

} // namespace

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : value_or(std::getenv("MQTT_HOST"), "localhost");
    const int port = argc > 2 ? std::stoi(argv[2]) : std::getenv("MQTT_PORT") ? std::stoi(std::getenv("MQTT_PORT")) : 1883;
    const char* topic = argc > 3 ? argv[3] : value_or(std::getenv("MQTT_TOPIC"), "msh/#");
    const int seconds = argc > 4 ? std::stoi(argv[4]) : 60;
    const char* username = value_or(std::getenv("MQTT_USERNAME"), "");
    const char* password = value_or(std::getenv("MQTT_PASSWORD"), "");

    mosquitto_lib_init();
    Probe probe;
    probe.topic = topic;
    mosquitto* client = mosquitto_new("meshat-monitor-probe", true, &probe);
    if (!client) {
        std::cerr << "unable to create MQTT client\n";
        mosquitto_lib_cleanup();
        return 1;
    }
    mosquitto_username_pw_set(client, username, password);
    mosquitto_connect_callback_set(client, on_connect);
    mosquitto_disconnect_callback_set(client, on_disconnect);
    mosquitto_message_callback_set(client, on_message);
    mosquitto_reconnect_delay_set(client, 1, 60, true);

    const int result = mosquitto_connect_async(client, host, port, 60);
    if (result != MOSQ_ERR_SUCCESS) {
        std::cerr << "connect_async failed: " << result << " (" << mosquitto_strerror(result) << ")\n";
        mosquitto_destroy(client);
        mosquitto_lib_cleanup();
        return 1;
    }
    if (mosquitto_loop_start(client) != MOSQ_ERR_SUCCESS) {
        std::cerr << "unable to start MQTT loop\n";
        mosquitto_destroy(client);
        mosquitto_lib_cleanup();
        return 1;
    }

    std::cout << "probing " << host << ':' << port << " for " << seconds << "s\n";
    for (int elapsed = 0; elapsed < seconds; ++elapsed) std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "summary: connected=" << probe.connected.load() << ", messages=" << probe.messages.load()
              << ", disconnects=" << probe.disconnects.load() << '\n';
    mosquitto_loop_stop(client, true);
    mosquitto_destroy(client);
    mosquitto_lib_cleanup();
    return 0;
}