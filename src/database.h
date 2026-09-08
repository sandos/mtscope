#pragma once

#include "packet.h"

#include <cstdint>
#include <mutex>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void insert(const std::string& topic, const void* payload, int length);
    void purge(int retention_days);
    std::string recent_json();
    std::string nodes_json();

private:
    void insert_measurement(std::int64_t packet_id, const Measurement& measurement);
    void add_column(const char* table, const char* name, const char* definition = "TEXT NOT NULL DEFAULT ''");
    void execute(const char* sql);
    void prepare(const char* sql, sqlite3_stmt** statement);

    sqlite3* db_ = nullptr;
    sqlite3_stmt* insert_ = nullptr;
    sqlite3_stmt* measurement_insert_ = nullptr;
    sqlite3_stmt* purge_ = nullptr;
    sqlite3_stmt* measurement_purge_ = nullptr;
    std::mutex mutex_;
};