#pragma once

#include <string>

struct Config {
    std::string host = "mqtt.meshat.se";
    int mqtt_port = 1883;
    std::string username = "msh";
    std::string password = "msh";
    std::string topic = "msh/#";
    std::string database = "meshat-monitor.db";
    std::string web_root = "web";
    int retention_days = 3;
    int http_port = 8099;
};