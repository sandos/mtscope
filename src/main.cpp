#include "app.h"
#include "config.h"
#include "database.h"
#include "http.h"
#include "monitor.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

volatile std::sig_atomic_t running = 1;

namespace {

void stop(int) { running = 0; }

Config parse_args(int argc, char** argv) {
    Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--help") {
            std::cout << "Usage: meshat-monitor [--database PATH] [--web-root PATH] [--topic MQTT_TOPIC] [--retention-days DAYS] [--http-port PORT]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) throw std::runtime_error("Missing value for " + option);
        const std::string value = argv[++index];
        if (option == "--database") config.database = value;
        else if (option == "--web-root") config.web_root = value;
        else if (option == "--retention-days") config.retention_days = std::stoi(value);
        else if (option == "--topic") config.topic = value;
        else if (option == "--http-port") config.http_port = std::stoi(value);
        else if (option == "--host") config.host = value;
        else if (option == "--mqtt-port") config.mqtt_port = std::stoi(value);
        else if (option == "--username") config.username = value;
        else if (option == "--password") config.password = value;
        else throw std::runtime_error("Unknown option: " + option);
    }
    if (config.retention_days < 1) throw std::runtime_error("--retention-days must be at least 1");
    return config;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Config config = parse_args(argc, argv);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        Database database(config.database);
        database.purge(config.retention_days);
        Monitor monitor(config, database);
        monitor.run();
        std::thread web([&] { serve_http(database, config.http_port, config.web_root); });
        auto next_purge = std::chrono::steady_clock::now() + std::chrono::hours(1);
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (std::chrono::steady_clock::now() >= next_purge) {
                database.purge(config.retention_days);
                next_purge += std::chrono::hours(1);
            }
        }
        web.join();
    } catch (const std::exception& error) {
        std::cerr << "meshat-monitor: " << error.what() << "\n";
        return 1;
    }
    return 0;
}