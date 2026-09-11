#include "http.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/select.h>
#include <unistd.h>

#include "app.h"
#include "database.h"
#include "logger.h"
#include "monitor.h"

namespace {

void send_response(int client, int status, const char* reason, const char* type, const std::string& body) {
    const std::string header = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + std::string(type) + "; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    send(client, header.data(), header.size(), MSG_NOSIGNAL);
    send(client, body.data(), body.size(), MSG_NOSIGNAL);
}

std::string load_asset(const std::string& web_root, const char* name) {
    std::ifstream file(web_root + "/" + name);
    if (!file) throw std::runtime_error("Unable to read dashboard asset " + web_root + "/" + name);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string json_escape(const std::string& value) {
    std::string output;
    for (const char character : value) {
        if (character == '\\') output += "\\\\";
        else if (character == '"') output += "\\\"";
        else output += character;
    }
    return output;
}

} // namespace

HttpServer::HttpServer(int port, const std::string& web_root, Logger& logger)
    : index_(load_asset(web_root, "index.html")),
      stylesheet_(load_asset(web_root, "styles.css")),
    script_(load_asset(web_root, "app.js")),
    logger_(logger),
      server_(socket(AF_INET, SOCK_STREAM, 0)) {
    if (server_ < 0) throw std::runtime_error("Unable to create HTTP socket");
    int enabled = 1;
    setsockopt(server_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(server_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(server_, 8) < 0) {
        throw std::runtime_error("Unable to listen on HTTP port");
    }
    std::cerr << "HTTP dashboard on port " << port << "\n";
    logger_.info("HTTP", "Listening on port " + std::to_string(port));
}

HttpServer::~HttpServer() {
    logger_.info("HTTP", "Server stopped");
    if (server_ >= 0) close(server_);
}

void HttpServer::record_api_timing(const std::string& route, double elapsed_ms) const {
    auto& timing = api_timings_[route];
    ++timing.calls;
    timing.total_ms += elapsed_ms;
    timing.last_ms = elapsed_ms;
    timing.max_ms = std::max(timing.max_ms, elapsed_ms);
}

std::string HttpServer::api_timings_json() const {
    std::string result = "[";
    bool first = true;
    for (const auto& [route, timing] : api_timings_) {
        if (!first) result += ',';
        first = false;
        result += "{\"route\":\"" + json_escape(route) + "\",\"calls\":" + std::to_string(timing.calls) +
            ",\"average_ms\":" + std::to_string(timing.total_ms / timing.calls) +
            ",\"last_ms\":" + std::to_string(timing.last_ms) +
            ",\"max_ms\":" + std::to_string(timing.max_ms) + "}";
    }
    return result + "]";
}

std::string HttpServer::client_stats_json() const {
    return "{\"connections\":" + std::to_string(client_connections_) +
        ",\"receive_timeouts\":" + std::to_string(client_timeouts_) +
        ",\"disconnections\":" + std::to_string(client_disconnections_) + "}";
}

void HttpServer::serve(Database& database, const Monitor& monitor) const {
    while (running) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(server_, &readable);
        timeval timeout{1, 0};
        if (select(server_ + 1, &readable, nullptr, nullptr, &timeout) <= 0) continue;
        const int client = accept(server_, nullptr, nullptr);
        if (client < 0) continue;
        ++client_connections_;
        logger_.info("HTTP", "Client connected");
        timeval receive_timeout{1, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
        char request[1024]{};
        const ssize_t size = recv(client, request, sizeof(request) - 1, 0);
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ++client_timeouts_;
        const std::string line(request, size > 0 ? static_cast<size_t>(size) : 0);
        const auto request_started = std::chrono::steady_clock::now();
        std::string api_route;
        if (line.rfind("GET /api/", 0) == 0) {
            const size_t route_end = line.find(' ', 5);
            api_route = line.substr(5, route_end == std::string::npos ? std::string::npos : route_end - 5);
        }
        try {
            if (line.rfind("GET /api/packets/", 0) == 0 && line.find("/observations ") != std::string::npos) {
                const size_t prefix = std::string("GET /api/packets/").size();
                const size_t suffix = line.find("/observations ", prefix);
                send_response(client, 200, "OK", "application/json", database.observations_json(line.substr(prefix, suffix - prefix)));
            } else if (line.rfind("GET /api/packets ", 0) == 0) send_response(client, 200, "OK", "application/json", database.recent_json());
            else if (line.rfind("GET /api/nodes ", 0) == 0) send_response(client, 200, "OK", "application/json", database.nodes_json());
            else if (line.rfind("GET /api/stats ", 0) == 0) {
                std::string stats = database.stats_json();
                stats.pop_back();
                stats += "," + resource_stats_.json() + ",\"api_timings\":" + api_timings_json() + ",\"http_clients\":" + client_stats_json() + "}";
                send_response(client, 200, "OK", "application/json", stats);
            }
            else if (line.rfind("GET /api/logs ", 0) == 0) send_response(client, 200, "OK", "application/json", logger_.json());
            else if (line.rfind("GET /api/status ", 0) == 0) send_response(client, 200, "OK", "application/json", monitor.connected() ? "{\"mqtt_connected\":true}" : "{\"mqtt_connected\":false}");
            else if (line.rfind("GET /styles.css ", 0) == 0) send_response(client, 200, "OK", "text/css", stylesheet_);
            else if (line.rfind("GET /app.js ", 0) == 0) send_response(client, 200, "OK", "application/javascript", script_);
            else if (line.rfind("GET / ", 0) == 0) send_response(client, 200, "OK", "text/html", index_);
            else send_response(client, 404, "Not Found", "application/json", "{\"error\":\"Not found\"}");
        } catch (const std::exception& error) {
            std::cerr << "HTTP request failed: " << error.what() << "\n";
            send_response(client, 500, "Internal Server Error", "application/json", "{\"error\":\"Internal server error\"}");
        }
        if (!api_route.empty()) {
            const double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - request_started).count();
            record_api_timing(api_route, elapsed_ms);
        }
        close(client);
        ++client_disconnections_;
        logger_.info("HTTP", "Client disconnected");
    }
}