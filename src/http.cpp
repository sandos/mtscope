#include "http.h"

#include <arpa/inet.h>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/select.h>
#include <unistd.h>

#include "app.h"
#include "database.h"

namespace {

void send_response(int client, const char* type, const std::string& body) {
    const std::string header = "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(type) + "; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    send(client, header.data(), header.size(), MSG_NOSIGNAL);
    send(client, body.data(), body.size(), MSG_NOSIGNAL);
}

std::string load_asset(const std::string& web_root, const char* name) {
    std::ifstream file(web_root + "/" + name);
    if (!file) throw std::runtime_error("Unable to read dashboard asset " + web_root + "/" + name);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

HttpServer::HttpServer(int port, const std::string& web_root)
    : index_(load_asset(web_root, "index.html")),
      stylesheet_(load_asset(web_root, "styles.css")),
      script_(load_asset(web_root, "app.js")),
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
}

HttpServer::~HttpServer() {
    if (server_ >= 0) close(server_);
}

void HttpServer::serve(Database& database) const {
    while (running) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(server_, &readable);
        timeval timeout{1, 0};
        if (select(server_ + 1, &readable, nullptr, nullptr, &timeout) <= 0) continue;
        const int client = accept(server_, nullptr, nullptr);
        if (client < 0) continue;
        char request[1024]{};
        const ssize_t size = recv(client, request, sizeof(request) - 1, 0);
        const std::string line(request, size > 0 ? static_cast<size_t>(size) : 0);
        if (line.rfind("GET /api/packets/", 0) == 0 && line.find("/observations ") != std::string::npos) {
            const size_t prefix = std::string("GET /api/packets/").size();
            const size_t suffix = line.find("/observations ", prefix);
            send_response(client, "application/json", database.observations_json(line.substr(prefix, suffix - prefix)));
        } else if (line.rfind("GET /api/packets ", 0) == 0) send_response(client, "application/json", database.recent_json());
        else if (line.rfind("GET /api/nodes ", 0) == 0) send_response(client, "application/json", database.nodes_json());
        else if (line.rfind("GET /styles.css ", 0) == 0) send_response(client, "text/css", stylesheet_);
        else if (line.rfind("GET /app.js ", 0) == 0) send_response(client, "application/javascript", script_);
        else send_response(client, "text/html", index_);
        close(client);
    }
}