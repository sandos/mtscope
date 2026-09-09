#pragma once

#include "config.h"

class Database;
struct mosquitto;

class Monitor {
public:
    Monitor(const Config& config, Database& database);
    ~Monitor();

    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    void run();

private:
    static void on_connect(mosquitto* client, void* context, int result);
    static void on_disconnect(mosquitto*, void* context, int result);
    static void on_message(mosquitto*, void* context, const struct mosquitto_message* message);

    const Config& config_;
    Database& database_;
    mosquitto* client_ = nullptr;
};