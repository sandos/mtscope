#pragma once

#include <string>

class Database;

class HttpServer {
public:
	HttpServer(int port, const std::string& web_root);
	~HttpServer();

	HttpServer(const HttpServer&) = delete;
	HttpServer& operator=(const HttpServer&) = delete;

	void serve(Database& database) const;

private:
	std::string index_;
	std::string stylesheet_;
	std::string script_;
	int server_ = -1;
};