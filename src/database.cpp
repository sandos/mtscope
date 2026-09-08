#include "database.h"

#include <ctime>
#include <iostream>
#include <stdexcept>

#include <sqlite3.h>

namespace {

std::int64_t now_seconds() { return std::time(nullptr); }

std::string json_escape(const unsigned char* value, int length) {
    if (!value) return {};
    std::string output;
    output.reserve(static_cast<size_t>(length) + 8);
    for (int index = 0; index < length; ++index) {
        const auto character = value[index];
        switch (character) {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default: output += character < 0x20 ? '?' : static_cast<char>(character);
        }
    }
    return output;
}

void bind_optional(sqlite3_stmt* statement, int index, const std::optional<double>& value) {
    if (value) sqlite3_bind_double(statement, index, *value);
    else sqlite3_bind_null(statement, index);
}

} // namespace

Database::Database(const std::string& path) {
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Unable to open SQLite database: " + std::string(sqlite3_errmsg(db_)));
    }
    execute("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA temp_store=MEMORY;");
    execute("CREATE TABLE IF NOT EXISTS packets ("
        "id INTEGER PRIMARY KEY, received_at INTEGER NOT NULL, topic TEXT NOT NULL, payload BLOB NOT NULL, "
        "region TEXT NOT NULL DEFAULT '', transport TEXT NOT NULL DEFAULT '', encoding TEXT NOT NULL DEFAULT '', "
        "channel TEXT NOT NULL DEFAULT '', node TEXT NOT NULL DEFAULT '', packet_type TEXT NOT NULL DEFAULT '', "
        "sender TEXT NOT NULL DEFAULT '', decoded_payload_hex TEXT NOT NULL DEFAULT '');");
    for (const char* column : {"region", "transport", "encoding", "channel", "node", "packet_type", "sender", "decoded_payload_hex"}) {
        add_column("packets", column);
    }
    execute("CREATE INDEX IF NOT EXISTS packets_received_at ON packets(received_at);");
    execute("CREATE TABLE IF NOT EXISTS measurements ("
        "id INTEGER PRIMARY KEY, packet_id INTEGER NOT NULL, received_at INTEGER NOT NULL, kind TEXT NOT NULL, "
        "node_id TEXT NOT NULL DEFAULT '', long_name TEXT NOT NULL DEFAULT '', short_name TEXT NOT NULL DEFAULT '', "
        "hardware_model TEXT NOT NULL DEFAULT '', role TEXT NOT NULL DEFAULT '', is_licensed INTEGER, is_unmessagable INTEGER, has_public_key INTEGER NOT NULL DEFAULT 0, "
        "text TEXT NOT NULL DEFAULT '', latitude REAL, longitude REAL, altitude REAL, battery_level REAL, "
        "voltage REAL, temperature REAL, relative_humidity REAL, pressure REAL);");
    add_column("measurements", "hardware_model");
    add_column("measurements", "role");
    add_column("measurements", "is_licensed", "INTEGER");
    add_column("measurements", "is_unmessagable", "INTEGER");
    add_column("measurements", "has_public_key", "INTEGER NOT NULL DEFAULT 0");
    execute("CREATE INDEX IF NOT EXISTS measurements_received_at ON measurements(received_at);");
    prepare("INSERT INTO packets(received_at, topic, payload, region, transport, encoding, channel, node, packet_type, sender, decoded_payload_hex) VALUES(?,?,?,?,?,?,?,?,?,?,?)", &insert_);
    prepare("INSERT INTO measurements(received_at, packet_id, kind, node_id, long_name, short_name, hardware_model, role, is_licensed, is_unmessagable, has_public_key, text, latitude, longitude, altitude, battery_level, voltage, temperature, relative_humidity, pressure) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &measurement_insert_);
    prepare("DELETE FROM packets WHERE received_at < ?", &purge_);
    prepare("DELETE FROM measurements WHERE received_at < ?", &measurement_purge_);
}

Database::~Database() {
    sqlite3_finalize(insert_);
    sqlite3_finalize(measurement_insert_);
    sqlite3_finalize(purge_);
    sqlite3_finalize(measurement_purge_);
    sqlite3_close(db_);
}

void Database::insert(const std::string& topic, const void* payload, int length) {
    const ParsedPacket parsed = parse_packet(topic, payload, length);
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_reset(insert_);
    sqlite3_clear_bindings(insert_);
    sqlite3_bind_int64(insert_, 1, now_seconds());
    sqlite3_bind_text(insert_, 2, topic.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(insert_, 3, payload, length, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_, 4, parsed.region.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_, 5, parsed.transport.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_, 6, parsed.encoding.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_, 7, parsed.channel.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_, 8, parsed.node.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_, 9, parsed.packet_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_, 10, parsed.sender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_, 11, parsed.decoded_payload_hex.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(insert_) != SQLITE_DONE) {
        std::cerr << "SQLite insert failed: " << sqlite3_errmsg(db_) << "\n";
        return;
    }
    if (parsed.measurement) insert_measurement(sqlite3_last_insert_rowid(db_), *parsed.measurement);
}

void Database::purge(int retention_days) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cutoff = now_seconds() - static_cast<std::int64_t>(retention_days) * 86400;
    sqlite3_reset(purge_);
    sqlite3_clear_bindings(purge_);
    sqlite3_bind_int64(purge_, 1, cutoff);
    if (sqlite3_step(purge_) != SQLITE_DONE) std::cerr << "SQLite purge failed: " << sqlite3_errmsg(db_) << "\n";
    sqlite3_reset(measurement_purge_);
    sqlite3_clear_bindings(measurement_purge_);
    sqlite3_bind_int64(measurement_purge_, 1, cutoff);
    if (sqlite3_step(measurement_purge_) != SQLITE_DONE) std::cerr << "SQLite measurement purge failed: " << sqlite3_errmsg(db_) << "\n";
}

std::string Database::recent_json() {
    constexpr const char* sql = "SELECT p.received_at, p.topic, p.payload, p.region, p.transport, p.encoding, p.channel, p.node, p.packet_type, p.sender, p.decoded_payload_hex, m.kind, m.node_id, m.long_name, m.short_name, m.hardware_model, m.role, m.is_licensed, m.is_unmessagable, m.has_public_key, m.text, m.latitude, m.longitude, m.altitude, m.battery_level, m.voltage, m.temperature, m.relative_humidity, m.pressure FROM packets p LEFT JOIN measurements m ON m.packet_id = p.id ORDER BY p.id DESC LIMIT 500";
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) return "[]";
    std::string result = "[";
    bool first = true;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        if (!first) result += ',';
        first = false;
        const auto text = [&](int column) { return json_escape(sqlite3_column_text(statement, column), sqlite3_column_bytes(statement, column)); };
        const auto number = [&](int column) { return sqlite3_column_type(statement, column) == SQLITE_NULL ? std::string("null") : std::to_string(sqlite3_column_double(statement, column)); };
        result += "{\"received_at\":" + std::to_string(sqlite3_column_int64(statement, 0));
        result += ",\"topic\":\"" + text(1) + "\",\"payload_hex\":\"" + hex_encode(sqlite3_column_blob(statement, 2), sqlite3_column_bytes(statement, 2)) + "\"";
        result += ",\"region\":\"" + text(3) + "\",\"transport\":\"" + text(4) + "\",\"encoding\":\"" + text(5) + "\",\"channel\":\"" + text(6) + "\",\"node\":\"" + text(7) + "\",\"packet_type\":\"" + text(8) + "\",\"sender\":\"" + text(9) + "\",\"decoded_payload_hex\":\"" + text(10) + "\"";
        if (sqlite3_column_type(statement, 11) != SQLITE_NULL) {
            result += ",\"measurement\":{\"kind\":\"" + text(11) + "\",\"node_id\":\"" + text(12) + "\",\"long_name\":\"" + text(13) + "\",\"short_name\":\"" + text(14) + "\",\"hardware_model\":\"" + text(15) + "\",\"role\":\"" + text(16) + "\",\"is_licensed\":" + number(17) + ",\"is_unmessagable\":" + number(18) + ",\"has_public_key\":" + number(19) + ",\"text\":\"" + text(20) + "\",\"latitude\":" + number(21) + ",\"longitude\":" + number(22) + ",\"altitude\":" + number(23) + ",\"battery_level\":" + number(24) + ",\"voltage\":" + number(25) + ",\"temperature\":" + number(26) + ",\"relative_humidity\":" + number(27) + ",\"pressure\":" + number(28) + "}";
        }
        result += "}";
    }
    sqlite3_finalize(statement);
    return result + "]";
}

std::string Database::nodes_json() {
    constexpr const char* sql = "SELECT m.node_id, m.long_name, m.short_name, m.hardware_model, m.role, m.is_licensed, m.is_unmessagable, m.has_public_key, m.received_at FROM measurements m INNER JOIN (SELECT node_id, MAX(received_at) AS last_seen FROM measurements WHERE kind = 'nodeinfo' AND node_id <> '' GROUP BY node_id) latest ON latest.node_id = m.node_id AND latest.last_seen = m.received_at WHERE m.kind = 'nodeinfo' ORDER BY m.received_at DESC LIMIT 500";
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) return "[]";
    std::string result = "[";
    bool first = true;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        if (!first) result += ',';
        first = false;
        const auto text = [&](int column) { return json_escape(sqlite3_column_text(statement, column), sqlite3_column_bytes(statement, column)); };
        const auto optional_integer = [&](int column) { return sqlite3_column_type(statement, column) == SQLITE_NULL ? std::string("null") : std::to_string(sqlite3_column_int(statement, column)); };
        result += "{\"node_id\":\"" + text(0) + "\",\"long_name\":\"" + text(1) + "\",\"short_name\":\"" + text(2) + "\",\"hardware_model\":\"" + text(3) + "\",\"role\":\"" + text(4) + "\",\"is_licensed\":" + optional_integer(5) + ",\"is_unmessagable\":" + optional_integer(6) + ",\"has_public_key\":" + std::to_string(sqlite3_column_int(statement, 7)) + ",\"last_seen\":" + std::to_string(sqlite3_column_int64(statement, 8)) + "}";
    }
    sqlite3_finalize(statement);
    return result + "]";
}

void Database::insert_measurement(std::int64_t packet_id, const Measurement& measurement) {
    sqlite3_reset(measurement_insert_);
    sqlite3_clear_bindings(measurement_insert_);
    sqlite3_bind_int64(measurement_insert_, 1, now_seconds());
    sqlite3_bind_int64(measurement_insert_, 2, packet_id);
    sqlite3_bind_text(measurement_insert_, 3, measurement.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(measurement_insert_, 4, measurement.node_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(measurement_insert_, 5, measurement.long_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(measurement_insert_, 6, measurement.short_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(measurement_insert_, 7, measurement.hardware_model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(measurement_insert_, 8, measurement.role.c_str(), -1, SQLITE_TRANSIENT);
    if (measurement.is_licensed) sqlite3_bind_int(measurement_insert_, 9, *measurement.is_licensed); else sqlite3_bind_null(measurement_insert_, 9);
    if (measurement.is_unmessagable) sqlite3_bind_int(measurement_insert_, 10, *measurement.is_unmessagable); else sqlite3_bind_null(measurement_insert_, 10);
    sqlite3_bind_int(measurement_insert_, 11, measurement.has_public_key);
    sqlite3_bind_text(measurement_insert_, 12, measurement.text.c_str(), -1, SQLITE_TRANSIENT);
    bind_optional(measurement_insert_, 13, measurement.latitude);
    bind_optional(measurement_insert_, 14, measurement.longitude);
    bind_optional(measurement_insert_, 15, measurement.altitude);
    bind_optional(measurement_insert_, 16, measurement.battery_level);
    bind_optional(measurement_insert_, 17, measurement.voltage);
    bind_optional(measurement_insert_, 18, measurement.temperature);
    bind_optional(measurement_insert_, 19, measurement.relative_humidity);
    bind_optional(measurement_insert_, 20, measurement.pressure);
    if (sqlite3_step(measurement_insert_) != SQLITE_DONE) std::cerr << "SQLite measurement insert failed: " << sqlite3_errmsg(db_) << "\n";
}

void Database::add_column(const char* table, const char* name, const char* definition) {
    const std::string sql = std::string("ALTER TABLE ") + table + " ADD COLUMN " + name + " " + definition;
    char* error = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : "unknown SQLite error";
        sqlite3_free(error);
        if (message.find("duplicate column name") == std::string::npos) throw std::runtime_error(message);
    }
}

void Database::execute(const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : "unknown SQLite error";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void Database::prepare(const char* sql, sqlite3_stmt** statement) {
    if (sqlite3_prepare_v2(db_, sql, -1, statement, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
}