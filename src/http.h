#pragma once

#include <string>

class Database;
class Monitor;

class HttpServer {
public:
	HttpServer(int port, const std::string& web_root);
	~HttpServer();

	HttpServer(const HttpServer&) = delete;
	HttpServer& operator=(const HttpServer&) = delete;

	void serve(Database& database, const Monitor& monitor) const;

private:
	std::string index_;
	std::string stylesheet_;
	std::string script_;
	int server_ = -1;
};