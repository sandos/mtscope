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

[[noreturn]] void throw_query_error(sqlite3* db, const char* operation) {
    throw std::runtime_error(std::string("SQLite ") + operation + " failed: " + sqlite3_errmsg(db));
}

} // namespace

Database::Database(const std::string& path) {
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Unable to open SQLite database: " + std::string(sqlite3_errmsg(db_)));
    }
    execute("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA temp_store=MEMORY;");
    execute("CREATE TABLE IF NOT EXISTS logical_packets ("
        "id INTEGER PRIMARY KEY, packet_key TEXT NOT NULL UNIQUE, sender TEXT NOT NULL DEFAULT '', destination TEXT NOT NULL DEFAULT '', "
        "mesh_packet_id INTEGER, channel TEXT NOT NULL DEFAULT '', packet_type TEXT NOT NULL DEFAULT '', logical_payload BLOB NOT NULL, "
        "decoded_payload_hex TEXT NOT NULL DEFAULT '', first_seen INTEGER NOT NULL, last_seen INTEGER NOT NULL);");
    execute("CREATE TABLE IF NOT EXISTS observations ("
        "id INTEGER PRIMARY KEY, logical_packet_id INTEGER NOT NULL, observed_at INTEGER NOT NULL, topic TEXT NOT NULL, payload BLOB NOT NULL, "
        "region TEXT NOT NULL DEFAULT '', transport TEXT NOT NULL DEFAULT '', encoding TEXT NOT NULL DEFAULT '', node TEXT NOT NULL DEFAULT '', observer TEXT NOT NULL DEFAULT '', "
        "rx_time INTEGER, rx_snr REAL, rx_rssi INTEGER, hop_limit INTEGER, hop_start INTEGER, via_mqtt INTEGER NOT NULL DEFAULT 0);");
    execute("CREATE INDEX IF NOT EXISTS observations_observed_at ON observations(observed_at);");
    execute("CREATE INDEX IF NOT EXISTS observations_logical_packet_id ON observations(logical_packet_id);");
    execute("CREATE INDEX IF NOT EXISTS observations_packet_time ON observations(logical_packet_id, observed_at DESC);");
    execute("CREATE TABLE IF NOT EXISTS measurements ("
        "id INTEGER PRIMARY KEY, logical_packet_id INTEGER NOT NULL, received_at INTEGER NOT NULL, kind TEXT NOT NULL, "
        "node_id TEXT NOT NULL DEFAULT '', long_name TEXT NOT NULL DEFAULT '', short_name TEXT NOT NULL DEFAULT '', "
        "hardware_model TEXT NOT NULL DEFAULT '', role TEXT NOT NULL DEFAULT '', is_licensed INTEGER, is_unmessagable INTEGER, has_public_key INTEGER NOT NULL DEFAULT 0, "
        "text TEXT NOT NULL DEFAULT '', latitude REAL, longitude REAL, altitude REAL, battery_level REAL, "
        "voltage REAL, temperature REAL, relative_humidity REAL, pressure REAL, UNIQUE(logical_packet_id, kind));");
    execute("CREATE INDEX IF NOT EXISTS measurements_received_at ON measurements(received_at);");
    execute("CREATE INDEX IF NOT EXISTS measurements_nodeinfo_lookup ON measurements(kind, node_id, received_at DESC, id DESC) WHERE kind = 'nodeinfo';");
    execute("CREATE INDEX IF NOT EXISTS measurements_location_lookup ON measurements(kind, node_id, received_at DESC, id DESC) WHERE kind IN ('position', 'map_report');");
    execute("CREATE INDEX IF NOT EXISTS logical_packets_status_lookup ON logical_packets(packet_type, sender, id);");
    execute("CREATE INDEX IF NOT EXISTS observations_packet_time ON observations(logical_packet_id, observed_at DESC);");
    execute("CREATE INDEX IF NOT EXISTS logical_status_sender_payload ON logical_packets(packet_type, sender, logical_payload);");
    prepare("INSERT OR IGNORE INTO logical_packets(packet_key, sender, destination, mesh_packet_id, channel, packet_type, logical_payload, decoded_payload_hex, first_seen, last_seen) VALUES(?,?,?,?,?,?,?,?,?,?)", &logical_insert_);
    prepare("UPDATE logical_packets SET last_seen = ?, packet_type = ?, decoded_payload_hex = ? WHERE packet_key = ?", &logical_update_);
    prepare("SELECT id FROM logical_packets WHERE packet_key = ?", &logical_select_);
    prepare("INSERT INTO observations(logical_packet_id, observed_at, topic, payload, region, transport, encoding, node, observer, rx_time, rx_snr, rx_rssi, hop_limit, hop_start, via_mqtt) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &observation_insert_);
    prepare("INSERT OR IGNORE INTO measurements(received_at, logical_packet_id, kind, node_id, long_name, short_name, hardware_model, role, is_licensed, is_unmessagable, has_public_key, text, latitude, longitude, altitude, battery_level, voltage, temperature, relative_humidity, pressure) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &measurement_insert_);
    prepare("DELETE FROM observations WHERE observed_at < ?", &purge_observations_);
    prepare("DELETE FROM measurements WHERE logical_packet_id NOT IN (SELECT DISTINCT logical_packet_id FROM observations)", &measurement_purge_);
    prepare("DELETE FROM logical_packets WHERE id NOT IN (SELECT DISTINCT logical_packet_id FROM observations)", &logical_purge_);
}

Database::~Database() {
    sqlite3_finalize(logical_insert_);
    sqlite3_finalize(logical_update_);
    sqlite3_finalize(logical_select_);
    sqlite3_finalize(observation_insert_);
    sqlite3_finalize(measurement_insert_);
    sqlite3_finalize(purge_observations_);
    sqlite3_finalize(measurement_purge_);
    sqlite3_finalize(logical_purge_);
    sqlite3_close(db_);
}

bool Database::insert(const std::string& topic, const void* payload, int length) {
    ParsedPacket parsed = parse_packet(topic, payload, length);
    std::lock_guard<std::mutex> lock(mutex_);
    char* begin_error = nullptr;
    if (sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, &begin_error) != SQLITE_OK) {
        std::cerr << "SQLite transaction start failed: " << (begin_error ? begin_error : sqlite3_errmsg(db_)) << "\n";
        sqlite3_free(begin_error);
        return false;
    }
    const auto fail = [this](const char* operation) {
        std::cerr << "SQLite " << operation << " failed: " << sqlite3_errmsg(db_) << "\n";
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    };
    const auto received_at = now_seconds();
    if (!parsed.mesh_packet_id) {
        parsed.packet_key += ":" + std::to_string(received_at) + ":" + std::to_string(++fallback_packet_sequence_);
    }
    sqlite3_reset(logical_insert_);
    sqlite3_clear_bindings(logical_insert_);
    sqlite3_bind_text(logical_insert_, 1, parsed.packet_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(logical_insert_, 2, parsed.sender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(logical_insert_, 3, parsed.destination.c_str(), -1, SQLITE_TRANSIENT);
    if (parsed.mesh_packet_id) sqlite3_bind_int64(logical_insert_, 4, *parsed.mesh_packet_id); else sqlite3_bind_null(logical_insert_, 4);
    sqlite3_bind_text(logical_insert_, 5, parsed.channel.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(logical_insert_, 6, parsed.packet_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(logical_insert_, 7, parsed.logical_payload.data(), static_cast<int>(parsed.logical_payload.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(logical_insert_, 8, parsed.decoded_payload_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(logical_insert_, 9, received_at);
    sqlite3_bind_int64(logical_insert_, 10, received_at);
    if (sqlite3_step(logical_insert_) != SQLITE_DONE) {
        return fail("logical packet insert");
    }
    sqlite3_reset(logical_update_);
    sqlite3_clear_bindings(logical_update_);
    sqlite3_bind_int64(logical_update_, 1, received_at);
    sqlite3_bind_text(logical_update_, 2, parsed.packet_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(logical_update_, 3, parsed.decoded_payload_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(logical_update_, 4, parsed.packet_key.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(logical_update_) != SQLITE_DONE) return fail("logical packet update");
    sqlite3_reset(logical_select_);
    sqlite3_clear_bindings(logical_select_);
    sqlite3_bind_text(logical_select_, 1, parsed.packet_key.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(logical_select_) != SQLITE_ROW) return fail("logical packet lookup");
    const auto logical_packet_id = sqlite3_column_int64(logical_select_, 0);
    sqlite3_reset(observation_insert_);
    sqlite3_clear_bindings(observation_insert_);
    sqlite3_bind_int64(observation_insert_, 1, logical_packet_id);
    sqlite3_bind_int64(observation_insert_, 2, received_at);
    sqlite3_bind_text(observation_insert_, 3, topic.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(observation_insert_, 4, payload, length, SQLITE_TRANSIENT);
    sqlite3_bind_text(observation_insert_, 5, parsed.region.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(observation_insert_, 6, parsed.transport.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(observation_insert_, 7, parsed.encoding.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(observation_insert_, 8, parsed.node.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(observation_insert_, 9, parsed.observer.c_str(), -1, SQLITE_TRANSIENT);
    if (parsed.rx_time) sqlite3_bind_int64(observation_insert_, 10, *parsed.rx_time); else sqlite3_bind_null(observation_insert_, 10);
    if (parsed.rx_snr) sqlite3_bind_double(observation_insert_, 11, *parsed.rx_snr); else sqlite3_bind_null(observation_insert_, 11);
    if (parsed.rx_rssi) sqlite3_bind_int(observation_insert_, 12, *parsed.rx_rssi); else sqlite3_bind_null(observation_insert_, 12);
    if (parsed.hop_limit) sqlite3_bind_int64(observation_insert_, 13, *parsed.hop_limit); else sqlite3_bind_null(observation_insert_, 13);
    if (parsed.hop_start) sqlite3_bind_int64(observation_insert_, 14, *parsed.hop_start); else sqlite3_bind_null(observation_insert_, 14);
    sqlite3_bind_int(observation_insert_, 15, parsed.via_mqtt);
    if (sqlite3_step(observation_insert_) != SQLITE_DONE) {
        return fail("observation insert");
    }
    if (parsed.measurement && !insert_measurement(logical_packet_id, received_at, *parsed.measurement)) {
        return fail("measurement insert");
    }
    char* commit_error = nullptr;
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &commit_error) != SQLITE_OK) {
        std::cerr << "SQLite transaction commit failed: " << (commit_error ? commit_error : sqlite3_errmsg(db_)) << "\n";
        sqlite3_free(commit_error);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

void Database::purge(int retention_days) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cutoff = now_seconds() - static_cast<std::int64_t>(retention_days) * 86400;
    sqlite3_reset(purge_observations_);
    sqlite3_clear_bindings(purge_observations_);
    sqlite3_bind_int64(purge_observations_, 1, cutoff);
    if (sqlite3_step(purge_observations_) != SQLITE_DONE) std::cerr << "SQLite observation purge failed: " << sqlite3_errmsg(db_) << "\n";
    sqlite3_reset(measurement_purge_);
    sqlite3_clear_bindings(measurement_purge_);
    if (sqlite3_step(measurement_purge_) != SQLITE_DONE) std::cerr << "SQLite measurement purge failed: " << sqlite3_errmsg(db_) << "\n";
    sqlite3_reset(logical_purge_);
    sqlite3_clear_bindings(logical_purge_);
    if (sqlite3_step(logical_purge_) != SQLITE_DONE) std::cerr << "SQLite logical packet purge failed: " << sqlite3_errmsg(db_) << "\n";
}

std::string Database::recent_json() {
    constexpr const char* sql = "SELECT o.observed_at, o.topic, o.payload, o.region, o.transport, o.encoding, o.node, o.observer, o.rx_time, o.rx_snr, o.rx_rssi, o.hop_limit, o.hop_start, o.via_mqtt, lp.packet_key, lp.sender, lp.destination, lp.mesh_packet_id, lp.channel, lp.packet_type, lp.logical_payload, lp.decoded_payload_hex, m.kind, m.node_id, m.long_name, m.short_name, m.hardware_model, m.role, m.is_licensed, m.is_unmessagable, m.has_public_key, m.text, m.latitude, m.longitude, m.altitude, m.battery_level, m.voltage, m.temperature, m.relative_humidity, m.pressure, (SELECT m2.short_name FROM measurements m2 WHERE m2.kind = 'nodeinfo' AND m2.node_id = lp.sender AND m2.received_at <= o.observed_at ORDER BY m2.received_at DESC, m2.id DESC LIMIT 1), (SELECT m2.short_name FROM measurements m2 WHERE m2.kind = 'nodeinfo' AND m2.node_id = o.observer AND m2.received_at <= o.observed_at ORDER BY m2.received_at DESC, m2.id DESC LIMIT 1), (SELECT m2.short_name FROM measurements m2 WHERE m2.kind = 'nodeinfo' AND m2.node_id = lp.destination AND m2.received_at <= o.observed_at ORDER BY m2.received_at DESC, m2.id DESC LIMIT 1), (SELECT COUNT(*) FROM observations o2 WHERE o2.logical_packet_id = lp.id) FROM observations o JOIN logical_packets lp ON lp.id = o.logical_packet_id LEFT JOIN measurements m ON m.logical_packet_id = lp.id WHERE lp.packet_type <> 'status' ORDER BY o.id DESC LIMIT 2000";
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) throw_query_error(db_, "recent packets query");
    std::string result = "[";
    bool first = true;
    int step_result = SQLITE_ROW;
    while ((step_result = sqlite3_step(statement)) == SQLITE_ROW) {
        if (!first) result += ',';
        first = false;
        const auto text = [&](int column) { return json_escape(sqlite3_column_text(statement, column), sqlite3_column_bytes(statement, column)); };
        const auto number = [&](int column) { return sqlite3_column_type(statement, column) == SQLITE_NULL ? std::string("null") : std::to_string(sqlite3_column_double(statement, column)); };
        result += "{\"received_at\":" + std::to_string(sqlite3_column_int64(statement, 0));
        result += ",\"topic\":\"" + text(1) + "\",\"payload_hex\":\"" + hex_encode(sqlite3_column_blob(statement, 2), sqlite3_column_bytes(statement, 2)) + "\"";
        result += ",\"region\":\"" + text(3) + "\",\"transport\":\"" + text(4) + "\",\"encoding\":\"" + text(5) + "\",\"node\":\"" + text(6) + "\",\"observer\":\"" + text(7) + "\",\"rx_time\":" + number(8) + ",\"rx_snr\":" + number(9) + ",\"rx_rssi\":" + number(10) + ",\"hop_limit\":" + number(11) + ",\"hop_start\":" + number(12) + ",\"via_mqtt\":" + std::to_string(sqlite3_column_int(statement, 13));
        result += ",\"packet_key\":\"" + text(14) + "\",\"sender\":\"" + text(15) + "\",\"destination\":\"" + text(16) + "\",\"mesh_packet_id\":" + number(17) + ",\"channel\":\"" + text(18) + "\",\"packet_type\":\"" + text(19) + "\",\"decoded_payload_hex\":\"" + text(21) + "\"";
        if (sqlite3_column_type(statement, 22) != SQLITE_NULL) {
            result += ",\"measurement\":{\"kind\":\"" + text(22) + "\",\"node_id\":\"" + text(23) + "\",\"long_name\":\"" + text(24) + "\",\"short_name\":\"" + text(25) + "\",\"hardware_model\":\"" + text(26) + "\",\"role\":\"" + text(27) + "\",\"is_licensed\":" + number(28) + ",\"is_unmessagable\":" + number(29) + ",\"has_public_key\":" + number(30) + ",\"text\":\"" + text(31) + "\",\"latitude\":" + number(32) + ",\"longitude\":" + number(33) + ",\"altitude\":" + number(34) + ",\"battery_level\":" + number(35) + ",\"voltage\":" + number(36) + ",\"temperature\":" + number(37) + ",\"relative_humidity\":" + number(38) + ",\"pressure\":" + number(39) + "}";
        }
        result += ",\"sender_name\":\"" + text(40) + "\",\"observer_name\":\"" + text(41) + "\",\"destination_name\":\"" + text(42) + "\"";
        result += ",\"observation_count\":" + std::to_string(sqlite3_column_int(statement, 43));
        result += "}";
    }
    if (step_result != SQLITE_DONE) {
        sqlite3_finalize(statement);
        throw_query_error(db_, "recent packets query");
    }
    sqlite3_finalize(statement);
    return result + "]";
}

std::string Database::observations_json(const std::string& packet_key) {
    constexpr const char* sql = "SELECT o.observed_at, o.topic, o.region, o.transport, o.encoding, o.node, o.observer, o.rx_time, o.rx_snr, o.rx_rssi, o.hop_limit, o.hop_start, o.via_mqtt FROM observations o JOIN logical_packets lp ON lp.id = o.logical_packet_id WHERE lp.packet_key = ? ORDER BY o.id";
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) throw_query_error(db_, "observations query");
    sqlite3_bind_text(statement, 1, packet_key.c_str(), -1, SQLITE_TRANSIENT);
    std::string result = "[";
    bool first = true;
    int step_result = SQLITE_ROW;
    while ((step_result = sqlite3_step(statement)) == SQLITE_ROW) {
        if (!first) result += ',';
        first = false;
        const auto text = [&](int column) { return json_escape(sqlite3_column_text(statement, column), sqlite3_column_bytes(statement, column)); };
        const auto number = [&](int column) { return sqlite3_column_type(statement, column) == SQLITE_NULL ? std::string("null") : std::to_string(sqlite3_column_double(statement, column)); };
        result += "{\"observed_at\":" + std::to_string(sqlite3_column_int64(statement, 0)) + ",\"topic\":\"" + text(1) + "\",\"region\":\"" + text(2) + "\",\"transport\":\"" + text(3) + "\",\"encoding\":\"" + text(4) + "\",\"node\":\"" + text(5) + "\",\"observer\":\"" + text(6) + "\",\"rx_time\":" + number(7) + ",\"rx_snr\":" + number(8) + ",\"rx_rssi\":" + number(9) + ",\"hop_limit\":" + number(10) + ",\"hop_start\":" + number(11) + ",\"via_mqtt\":" + std::to_string(sqlite3_column_int(statement, 12)) + "}";
    }
    if (step_result != SQLITE_DONE) {
        sqlite3_finalize(statement);
        throw_query_error(db_, "observations query");
    }
    sqlite3_finalize(statement);
    return result + "]";
}

std::string Database::nodes_json() {
    constexpr const char* sql = "SELECT m.node_id, m.long_name, m.short_name, m.hardware_model, m.role, m.is_licensed, m.is_unmessagable, m.has_public_key, MAX(m.received_at, COALESCE((SELECT MAX(o.observed_at) FROM observations o JOIN logical_packets lp ON lp.id = o.logical_packet_id WHERE lp.packet_type = 'status' AND lp.sender = m.node_id AND CAST(lp.logical_payload AS TEXT) = 'online'), m.received_at)), location.latitude, location.longitude, location.altitude FROM measurements m INNER JOIN (SELECT node_id, MAX(received_at) AS last_seen FROM measurements WHERE kind = 'nodeinfo' AND node_id <> '' GROUP BY node_id) latest ON latest.node_id = m.node_id AND latest.last_seen = m.received_at LEFT JOIN (SELECT p.node_id, p.latitude, p.longitude, p.altitude FROM measurements p INNER JOIN (SELECT node_id, MAX(received_at) AS last_seen FROM measurements WHERE kind IN ('position', 'map_report') AND node_id <> '' GROUP BY node_id) latest_location ON latest_location.node_id = p.node_id AND latest_location.last_seen = p.received_at WHERE p.kind IN ('position', 'map_report')) location ON location.node_id = m.node_id WHERE m.kind = 'nodeinfo' ORDER BY 9 DESC LIMIT 500";
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) throw_query_error(db_, "nodes query");
    std::string result = "[";
    bool first = true;
    int step_result = SQLITE_ROW;
    while ((step_result = sqlite3_step(statement)) == SQLITE_ROW) {
        if (!first) result += ',';
        first = false;
        const auto text = [&](int column) { return json_escape(sqlite3_column_text(statement, column), sqlite3_column_bytes(statement, column)); };
        const auto optional_integer = [&](int column) { return sqlite3_column_type(statement, column) == SQLITE_NULL ? std::string("null") : std::to_string(sqlite3_column_int(statement, column)); };
        const auto optional_number = [&](int column) { return sqlite3_column_type(statement, column) == SQLITE_NULL ? std::string("null") : std::to_string(sqlite3_column_double(statement, column)); };
        result += "{\"node_id\":\"" + text(0) + "\",\"long_name\":\"" + text(1) + "\",\"short_name\":\"" + text(2) + "\",\"hardware_model\":\"" + text(3) + "\",\"role\":\"" + text(4) + "\",\"is_licensed\":" + optional_integer(5) + ",\"is_unmessagable\":" + optional_integer(6) + ",\"has_public_key\":" + std::to_string(sqlite3_column_int(statement, 7)) + ",\"last_seen\":" + std::to_string(sqlite3_column_int64(statement, 8)) + ",\"latitude\":" + optional_number(9) + ",\"longitude\":" + optional_number(10) + ",\"altitude\":" + optional_number(11) + "}";
    }
    if (step_result != SQLITE_DONE) {
        sqlite3_finalize(statement);
        throw_query_error(db_, "nodes query");
    }
    sqlite3_finalize(statement);
    return result + "]";
}

std::string Database::stats_json() {
    constexpr const char* sql = "SELECT (SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size()), (SELECT COUNT(*) FROM logical_packets), (SELECT COUNT(*) FROM observations), (SELECT COUNT(*) FROM measurements), (SELECT COUNT(DISTINCT node_id) FROM measurements WHERE kind = 'nodeinfo' AND node_id <> ''), (SELECT MIN(observed_at) FROM observations), (SELECT MAX(observed_at) FROM observations)";
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) throw_query_error(db_, "database stats query");
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        throw_query_error(db_, "database stats query");
    }
    const auto optional_integer = [&](int column) { return sqlite3_column_type(statement, column) == SQLITE_NULL ? std::string("null") : std::to_string(sqlite3_column_int64(statement, column)); };
    const std::string result = "{\"database_bytes\":" + optional_integer(0) + ",\"logical_packets\":" + optional_integer(1) + ",\"observations\":" + optional_integer(2) + ",\"measurements\":" + optional_integer(3) + ",\"known_nodes\":" + optional_integer(4) + ",\"first_observed_at\":" + optional_integer(5) + ",\"last_observed_at\":" + optional_integer(6) + "}";
    sqlite3_finalize(statement);
    return result;
}

bool Database::insert_measurement(std::int64_t logical_packet_id, std::int64_t received_at, const Measurement& measurement) {
    sqlite3_reset(measurement_insert_);
    sqlite3_clear_bindings(measurement_insert_);
    sqlite3_bind_int64(measurement_insert_, 1, received_at);
    sqlite3_bind_int64(measurement_insert_, 2, logical_packet_id);
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
    return sqlite3_step(measurement_insert_) == SQLITE_DONE;
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