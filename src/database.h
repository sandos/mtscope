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
    std::string observations_json(const std::string& packet_key);
    std::string nodes_json();

private:
    void insert_measurement(std::int64_t logical_packet_id, std::int64_t received_at, const Measurement& measurement);
    void execute(const char* sql);
    void prepare(const char* sql, sqlite3_stmt** statement);

    sqlite3* db_ = nullptr;
    sqlite3_stmt* logical_insert_ = nullptr;
    sqlite3_stmt* logical_update_ = nullptr;
    sqlite3_stmt* logical_select_ = nullptr;
    sqlite3_stmt* observation_insert_ = nullptr;
    sqlite3_stmt* measurement_insert_ = nullptr;
    sqlite3_stmt* purge_observations_ = nullptr;
    sqlite3_stmt* measurement_purge_ = nullptr;
    sqlite3_stmt* logical_purge_ = nullptr;
    std::mutex mutex_;
};