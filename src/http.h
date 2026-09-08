#pragma once

#include <string>

class Database;

void serve_http(Database& database, int port, const std::string& web_root);