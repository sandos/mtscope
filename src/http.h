#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>

#include "stats.h"

class Database;
class Logger;
class Monitor;

class HttpServer {
public:
	HttpServer(int port, const std::string& web_root, Logger& logger);
	~HttpServer();

	HttpServer(const HttpServer&) = delete;
	HttpServer& operator=(const HttpServer&) = delete;

	void serve(Database& database, const Monitor& monitor) const;

private:
	struct ApiTiming {
		std::uint64_t calls = 0;
		double total_ms = 0;
		double last_ms = 0;
		double max_ms = 0;
	};

	void record_api_timing(const std::string& route, double elapsed_ms) const;
	std::string api_timings_json() const;
	std::string client_stats_json() const;

	std::string index_;
	std::string stylesheet_;
	std::string script_;
	mutable ResourceStats resource_stats_;
	Logger& logger_;
	mutable std::map<std::string, ApiTiming> api_timings_;
	mutable std::uint64_t client_connections_ = 0;
	mutable std::uint64_t client_timeouts_ = 0;
	mutable std::uint64_t client_disconnections_ = 0;
	int server_ = -1;
};