#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "meshtastic/mqtt.pb.h"
#include <mosquitto.h>
#ifdef MTSCOPE_HAVE_OPENSSL
#include <openssl/evp.h>
#endif
#include <sqlite3.h>

namespace {

struct Config {
    std::string host = "mqtt.meshat.se";
    int mqtt_port = 1883;
    std::string username = "msh";
    std::string password = "msh";
    std::string topic = "msh/#";
    std::string database = "meshat-monitor.db";
    int retention_days = 3;
    int http_port = 8099;
};

volatile std::sig_atomic_t running = 1;

void stop(int) { running = 0; }

std::int64_t now_seconds() { return std::time(nullptr); }

std::string json_escape(const unsigned char* value, int length) {
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
        default:
            if (character < 0x20) output += "?";
            else output += static_cast<char>(character);
        }
    }
    return output;
}

std::string hex_encode(const void* data, int length) {
    static constexpr char digits[] = "0123456789abcdef";
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::string output;
    output.reserve(static_cast<size_t>(length) * 2);
    for (int index = 0; index < length; ++index) {
        output += digits[bytes[index] >> 4];
        output += digits[bytes[index] & 0x0f];
    }
    return output;
}

std::string node_id(std::uint32_t value) {
    char buffer[10];
    std::snprintf(buffer, sizeof(buffer), "!%08x", value);
    return buffer;
}

struct Measurement {
    std::string kind;
    std::string text;
    std::string node_id;
    std::string long_name;
    std::string short_name;
    std::optional<double> latitude;
    std::optional<double> longitude;
    std::optional<double> altitude;
    std::optional<double> battery_level;
    std::optional<double> voltage;
    std::optional<double> temperature;
    std::optional<double> relative_humidity;
    std::optional<double> pressure;
};

struct ParsedPacket {
    std::string region;
    std::string transport;
    std::string encoding;
    std::string channel;
    std::string node;
    std::string packet_type;
    std::string sender;
    std::string decoded_payload_hex;
    std::optional<Measurement> measurement;
};

std::vector<std::string> split_topic(const std::string& topic) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= topic.size()) {
        const size_t end = topic.find('/', start);
        parts.push_back(topic.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parts;
}

std::string json_field(const std::string& json, const std::string& name) {
    const std::string key = "\"" + name + "\"";
    size_t key_position = json.find(key);
    while (key_position != std::string::npos) {
        size_t separator = key_position + key.size();
        while (separator < json.size() && std::isspace(static_cast<unsigned char>(json[separator]))) ++separator;
        if (separator < json.size() && json[separator] == ':') break;
        key_position = json.find(key, key_position + 1);
    }
    if (key_position == std::string::npos) return {};
    size_t value_position = json.find(':', key_position + key.size());
    if (value_position == std::string::npos) return {};
    ++value_position;
    while (value_position < json.size() && std::isspace(static_cast<unsigned char>(json[value_position]))) ++value_position;
    if (value_position >= json.size()) return {};
    if (json[value_position] == '"') {
        std::string value;
        for (size_t index = value_position + 1; index < json.size(); ++index) {
            if (json[index] == '"') return value;
            if (json[index] == '\\' && index + 1 < json.size()) ++index;
            value += json[index];
        }
        return {};
    }
    const size_t end = json.find_first_of(",}", value_position);
    const std::string value = json.substr(value_position, end == std::string::npos ? end : end - value_position);
    const size_t first = value.find_first_not_of(" \t\r\n");
    const size_t last = value.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : value.substr(first, last - first + 1);
}

std::optional<double> json_number(const std::string& json, const std::string& name) {
    const std::string value = json_field(json, name);
    if (value.empty()) return std::nullopt;
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool decode_data(ParsedPacket& packet, const meshtastic::Data& data) {
    packet.packet_type = meshtastic::PortNum_Name(data.portnum());
    packet.decoded_payload_hex = hex_encode(data.payload().data(), static_cast<int>(data.payload().size()));
    const std::string& data_payload = data.payload();
    if (data.portnum() == meshtastic::TEXT_MESSAGE_APP) {
        Measurement measurement;
        measurement.kind = "text";
        measurement.text = data_payload;
        measurement.node_id = packet.sender;
        packet.measurement = measurement;
    } else if (data.portnum() == meshtastic::POSITION_APP) {
        meshtastic::Position position;
        if (!position.ParseFromString(data_payload)) return false;
        Measurement measurement;
        measurement.kind = "position";
        measurement.node_id = packet.sender;
        if (position.has_latitude_i()) measurement.latitude = position.latitude_i() * 1e-7;
        if (position.has_longitude_i()) measurement.longitude = position.longitude_i() * 1e-7;
        if (position.has_altitude()) measurement.altitude = position.altitude();
        packet.measurement = measurement;
    } else if (data.portnum() == meshtastic::NODEINFO_APP) {
        meshtastic::User user;
        if (!user.ParseFromString(data_payload)) return false;
        Measurement measurement;
        measurement.kind = "nodeinfo";
        measurement.node_id = user.id().empty() ? packet.sender : user.id();
        measurement.long_name = user.long_name();
        measurement.short_name = user.short_name();
        packet.measurement = measurement;
    } else if (data.portnum() == meshtastic::TELEMETRY_APP) {
        meshtastic::Telemetry telemetry;
        if (!telemetry.ParseFromString(data_payload)) return false;
        Measurement measurement;
        measurement.kind = "telemetry";
        measurement.node_id = packet.sender;
        if (telemetry.has_device_metrics()) {
            const auto& metrics = telemetry.device_metrics();
            if (metrics.has_battery_level()) measurement.battery_level = metrics.battery_level();
            if (metrics.has_voltage()) measurement.voltage = metrics.voltage();
        }
        if (telemetry.has_environment_metrics()) {
            const auto& metrics = telemetry.environment_metrics();
            if (metrics.has_temperature()) measurement.temperature = metrics.temperature();
            if (metrics.has_relative_humidity()) measurement.relative_humidity = metrics.relative_humidity();
            if (metrics.has_barometric_pressure()) measurement.pressure = metrics.barometric_pressure();
            if (metrics.has_voltage()) measurement.voltage = metrics.voltage();
        }
        packet.measurement = measurement;
    }
    return packet.measurement.has_value();
}

#ifdef MTSCOPE_HAVE_OPENSSL
bool decrypt_default_channel(const meshtastic::MeshPacket& mesh_packet, std::string& plaintext) {
    static constexpr std::array<unsigned char, 16> key = {
        0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
        0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
    };
    std::array<unsigned char, 16> nonce{};
    const std::uint64_t packet_id = mesh_packet.id();
    const std::uint32_t sender = mesh_packet.from();
    std::memcpy(nonce.data(), &packet_id, sizeof(packet_id));
    std::memcpy(nonce.data() + sizeof(packet_id), &sender, sizeof(sender));

    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (!context) return false;
    const bool initialized = EVP_EncryptInit_ex(context, EVP_aes_128_ctr(), nullptr, key.data(), nonce.data()) == 1;
    std::string output(mesh_packet.encrypted().size(), '\0');
    int output_length = 0;
    int final_length = 0;
    const bool decrypted = initialized &&
        EVP_EncryptUpdate(context, reinterpret_cast<unsigned char*>(output.data()), &output_length,
            reinterpret_cast<const unsigned char*>(mesh_packet.encrypted().data()), static_cast<int>(mesh_packet.encrypted().size())) == 1 &&
        EVP_EncryptFinal_ex(context, reinterpret_cast<unsigned char*>(output.data()) + output_length, &final_length) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!decrypted) return false;
    output.resize(static_cast<size_t>(output_length + final_length));
    plaintext = std::move(output);
    return true;
}
#endif

ParsedPacket parse_packet(const std::string& topic, const void* payload, int length) {
    ParsedPacket packet;
    const auto parts = split_topic(topic);
    if (parts.size() > 1) packet.region = parts[1];
    if (parts.size() > 2) packet.transport = parts[2];
    if (parts.size() > 3) packet.encoding = parts[3];
    if (parts.size() > 4 && (packet.encoding == "json" || packet.encoding == "e" || packet.encoding == "c")) packet.channel = parts[4];
    if (!parts.empty() && !parts.back().empty() && parts.back()[0] == '!') packet.node = parts.back();

    const auto* bytes = static_cast<const unsigned char*>(payload);
    if (length > 0 && bytes[0] == '{') {
        const std::string json(static_cast<const char*>(payload), static_cast<size_t>(length));
        packet.packet_type = json_field(json, "type");
        if (packet.packet_type.empty()) packet.packet_type = json_field(json, "$typeName");
        if (packet.packet_type.empty() && !json_field(json, "text").empty()) packet.packet_type = "text";
        packet.sender = json_field(json, "sender");
        if (packet.channel.empty()) packet.channel = json_field(json, "channel");
        if (packet.packet_type == "text") {
            Measurement measurement;
            measurement.kind = "text";
            measurement.text = json_field(json, "text");
            measurement.node_id = packet.sender;
            packet.measurement = measurement;
        } else if (packet.packet_type == "telemetry" || packet.packet_type.find("Telemetry") != std::string::npos) {
            Measurement measurement;
            measurement.kind = "telemetry";
            measurement.node_id = packet.sender;
            measurement.battery_level = json_number(json, "batteryLevel");
            measurement.voltage = json_number(json, "voltage");
            measurement.temperature = json_number(json, "temperature");
            measurement.relative_humidity = json_number(json, "relativeHumidity");
            measurement.pressure = json_number(json, "barometricPressure");
            packet.measurement = measurement;
        } else if (packet.packet_type == "position") {
            Measurement measurement;
            measurement.kind = "position";
            measurement.node_id = packet.sender;
            measurement.latitude = json_number(json, "latitude");
            measurement.longitude = json_number(json, "longitude");
            measurement.altitude = json_number(json, "altitude");
            packet.measurement = measurement;
        } else if (packet.packet_type == "nodeinfo") {
            Measurement measurement;
            measurement.kind = "nodeinfo";
            measurement.node_id = packet.sender;
            measurement.long_name = json_field(json, "longName");
            measurement.short_name = json_field(json, "shortName");
            packet.measurement = measurement;
        }
    } else if (packet.encoding == "stat") {
        packet.packet_type = "status";
        packet.sender = packet.node;
    } else {
        meshtastic::ServiceEnvelope envelope;
        if (envelope.ParseFromArray(payload, length) && envelope.has_packet()) {
            const auto& mesh_packet = envelope.packet();
            packet.sender = node_id(mesh_packet.from());
            if (mesh_packet.has_decoded()) {
                decode_data(packet, mesh_packet.decoded());
            } else if (mesh_packet.has_encrypted()) {
                packet.packet_type = "encrypted";
#ifdef MTSCOPE_HAVE_OPENSSL
                std::string plaintext;
                meshtastic::Data data;
                if (decrypt_default_channel(mesh_packet, plaintext) && data.ParseFromString(plaintext) &&
                    decode_data(packet, data)) {
                    packet.packet_type = "decrypted_" + packet.packet_type;
                }
#endif
            }
        }
    }
    return packet;
}

class Database {
public:
    explicit Database(const std::string& path) {
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
            throw std::runtime_error("Unable to open SQLite database: " + std::string(sqlite3_errmsg(db_)));
        }
        execute("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA temp_store=MEMORY;");
        execute("CREATE TABLE IF NOT EXISTS packets ("
            "id INTEGER PRIMARY KEY, received_at INTEGER NOT NULL, topic TEXT NOT NULL, payload BLOB NOT NULL, "
            "region TEXT NOT NULL DEFAULT '', transport TEXT NOT NULL DEFAULT '', encoding TEXT NOT NULL DEFAULT '', "
            "channel TEXT NOT NULL DEFAULT '', node TEXT NOT NULL DEFAULT '', packet_type TEXT NOT NULL DEFAULT '', "
            "sender TEXT NOT NULL DEFAULT '', decoded_payload_hex TEXT NOT NULL DEFAULT '');");
        add_column("region");
        add_column("transport");
        add_column("encoding");
        add_column("channel");
        add_column("node");
        add_column("packet_type");
        add_column("sender");
        add_column("decoded_payload_hex");
        execute("CREATE INDEX IF NOT EXISTS packets_received_at ON packets(received_at);");
        execute("CREATE TABLE IF NOT EXISTS measurements ("
            "id INTEGER PRIMARY KEY, packet_id INTEGER NOT NULL, received_at INTEGER NOT NULL, kind TEXT NOT NULL, "
            "node_id TEXT NOT NULL DEFAULT '', long_name TEXT NOT NULL DEFAULT '', short_name TEXT NOT NULL DEFAULT '', "
            "text TEXT NOT NULL DEFAULT '', latitude REAL, longitude REAL, altitude REAL, battery_level REAL, "
            "voltage REAL, temperature REAL, relative_humidity REAL, pressure REAL);");
        execute("CREATE INDEX IF NOT EXISTS measurements_received_at ON measurements(received_at);");
        prepare("INSERT INTO packets(received_at, topic, payload, region, transport, encoding, channel, node, packet_type, sender, decoded_payload_hex) VALUES(?,?,?,?,?,?,?,?,?,?,?)", &insert_);
        prepare("INSERT INTO measurements(received_at, packet_id, kind, node_id, long_name, short_name, text, latitude, longitude, altitude, battery_level, voltage, temperature, relative_humidity, pressure) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &measurement_insert_);
        prepare("DELETE FROM packets WHERE received_at < ?", &purge_);
        prepare("DELETE FROM measurements WHERE received_at < ?", &measurement_purge_);
    }

    ~Database() {
        sqlite3_finalize(insert_);
        sqlite3_finalize(measurement_insert_);
        sqlite3_finalize(purge_);
        sqlite3_finalize(measurement_purge_);
        sqlite3_close(db_);
    }

    void insert(const std::string& topic, const void* payload, int length) {
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
        if (parsed.measurement) insert_measurement(static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_)), parsed.measurement.value());
    }

    void purge(int retention_days) {
        std::lock_guard<std::mutex> lock(mutex_);
        sqlite3_reset(purge_);
        sqlite3_bind_int64(purge_, 1, now_seconds() - static_cast<std::int64_t>(retention_days) * 86400);
        if (sqlite3_step(purge_) != SQLITE_DONE) std::cerr << "SQLite purge failed: " << sqlite3_errmsg(db_) << "\n";
        sqlite3_reset(measurement_purge_);
        sqlite3_clear_bindings(measurement_purge_);
        sqlite3_bind_int64(measurement_purge_, 1, now_seconds() - static_cast<std::int64_t>(retention_days) * 86400);
        if (sqlite3_step(measurement_purge_) != SQLITE_DONE) std::cerr << "SQLite measurement purge failed: " << sqlite3_errmsg(db_) << "\n";
    }

    std::string recent_json() {
        constexpr const char* sql = "SELECT p.received_at, p.topic, p.payload, p.region, p.transport, p.encoding, p.channel, p.node, p.packet_type, p.sender, p.decoded_payload_hex, m.kind, m.node_id, m.long_name, m.short_name, m.text, m.latitude, m.longitude, m.altitude, m.battery_level, m.voltage, m.temperature, m.relative_humidity, m.pressure FROM packets p LEFT JOIN measurements m ON m.packet_id = p.id ORDER BY p.id DESC LIMIT 100";
        std::lock_guard<std::mutex> lock(mutex_);
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) return "[]";
        std::string result = "[";
        bool first = true;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            if (!first) result += ',';
            first = false;
            result += "{\"received_at\":" + std::to_string(sqlite3_column_int64(statement, 0));
            result += ",\"topic\":\"" + json_escape(sqlite3_column_text(statement, 1), sqlite3_column_bytes(statement, 1)) + "\"";
            result += ",\"payload_hex\":\"" + hex_encode(sqlite3_column_blob(statement, 2), sqlite3_column_bytes(statement, 2)) + "\"";
            result += ",\"region\":\"" + json_escape(sqlite3_column_text(statement, 3), sqlite3_column_bytes(statement, 3)) + "\"";
            result += ",\"transport\":\"" + json_escape(sqlite3_column_text(statement, 4), sqlite3_column_bytes(statement, 4)) + "\"";
            result += ",\"encoding\":\"" + json_escape(sqlite3_column_text(statement, 5), sqlite3_column_bytes(statement, 5)) + "\"";
            result += ",\"channel\":\"" + json_escape(sqlite3_column_text(statement, 6), sqlite3_column_bytes(statement, 6)) + "\"";
            result += ",\"node\":\"" + json_escape(sqlite3_column_text(statement, 7), sqlite3_column_bytes(statement, 7)) + "\"";
            result += ",\"packet_type\":\"" + json_escape(sqlite3_column_text(statement, 8), sqlite3_column_bytes(statement, 8)) + "\"";
            result += ",\"sender\":\"" + json_escape(sqlite3_column_text(statement, 9), sqlite3_column_bytes(statement, 9)) + "\"";
            result += ",\"decoded_payload_hex\":\"" + json_escape(sqlite3_column_text(statement, 10), sqlite3_column_bytes(statement, 10)) + "\"";
            if (sqlite3_column_type(statement, 11) != SQLITE_NULL) {
                auto measurement_text = [&](int column) {
                    const auto* value = sqlite3_column_text(statement, column);
                    return value ? json_escape(value, sqlite3_column_bytes(statement, column)) : std::string{};
                };
                auto measurement_number = [&](int column) {
                    return sqlite3_column_type(statement, column) == SQLITE_NULL ? std::string("null") : std::to_string(sqlite3_column_double(statement, column));
                };
                result += ",\"measurement\":{";
                result += "\"kind\":\"" + measurement_text(11) + "\"";
                result += ",\"node_id\":\"" + measurement_text(12) + "\"";
                result += ",\"long_name\":\"" + measurement_text(13) + "\"";
                result += ",\"short_name\":\"" + measurement_text(14) + "\"";
                result += ",\"text\":\"" + measurement_text(15) + "\"";
                result += ",\"latitude\":" + measurement_number(16);
                result += ",\"longitude\":" + measurement_number(17);
                result += ",\"altitude\":" + measurement_number(18);
                result += ",\"battery_level\":" + measurement_number(19);
                result += ",\"voltage\":" + measurement_number(20);
                result += ",\"temperature\":" + measurement_number(21);
                result += ",\"relative_humidity\":" + measurement_number(22);
                result += ",\"pressure\":" + measurement_number(23) + "}";
            }
            result += "}";
        }
        sqlite3_finalize(statement);
        return result + "]";
    }

private:
    static void bind_optional(sqlite3_stmt* statement, int index, const std::optional<double>& value) {
        if (value) sqlite3_bind_double(statement, index, *value);
        else sqlite3_bind_null(statement, index);
    }

    void insert_measurement(std::int64_t packet_id, const Measurement& measurement) {
        sqlite3_reset(measurement_insert_);
        sqlite3_clear_bindings(measurement_insert_);
        sqlite3_bind_int64(measurement_insert_, 1, now_seconds());
        sqlite3_bind_int64(measurement_insert_, 2, packet_id);
        sqlite3_bind_text(measurement_insert_, 3, measurement.kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(measurement_insert_, 4, measurement.node_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(measurement_insert_, 5, measurement.long_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(measurement_insert_, 6, measurement.short_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(measurement_insert_, 7, measurement.text.c_str(), -1, SQLITE_TRANSIENT);
        bind_optional(measurement_insert_, 8, measurement.latitude);
        bind_optional(measurement_insert_, 9, measurement.longitude);
        bind_optional(measurement_insert_, 10, measurement.altitude);
        bind_optional(measurement_insert_, 11, measurement.battery_level);
        bind_optional(measurement_insert_, 12, measurement.voltage);
        bind_optional(measurement_insert_, 13, measurement.temperature);
        bind_optional(measurement_insert_, 14, measurement.relative_humidity);
        bind_optional(measurement_insert_, 15, measurement.pressure);
        if (sqlite3_step(measurement_insert_) != SQLITE_DONE) std::cerr << "SQLite measurement insert failed: " << sqlite3_errmsg(db_) << "\n";
    }

    void add_column(const char* name) {
        const std::string sql = std::string("ALTER TABLE packets ADD COLUMN ") + name + " TEXT NOT NULL DEFAULT ''";
        char* error = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : "unknown SQLite error";
            sqlite3_free(error);
            if (message.find("duplicate column name") == std::string::npos) throw std::runtime_error(message);
        }
    }

    void execute(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            std::string message = error ? error : "unknown SQLite error";
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

    void prepare(const char* sql, sqlite3_stmt** statement) {
        if (sqlite3_prepare_v2(db_, sql, -1, statement, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
    }

    sqlite3* db_ = nullptr;
    sqlite3_stmt* insert_ = nullptr;
    sqlite3_stmt* measurement_insert_ = nullptr;
    sqlite3_stmt* purge_ = nullptr;
    sqlite3_stmt* measurement_purge_ = nullptr;
    std::mutex mutex_;
};

class Monitor {
public:
    Monitor(const Config& config, Database& database) : config_(config), database_(database) {}

    void run() {
        mosquitto_lib_init();
        client_ = mosquitto_new("meshat-monitor", true, this);
        if (!client_) throw std::runtime_error("Unable to create MQTT client");
        mosquitto_username_pw_set(client_, config_.username.c_str(), config_.password.c_str());
        mosquitto_connect_callback_set(client_, on_connect);
        mosquitto_message_callback_set(client_, on_message);
        mosquitto_reconnect_delay_set(client_, 1, 60, true);
        if (mosquitto_connect_async(client_, config_.host.c_str(), config_.mqtt_port, 60) != MOSQ_ERR_SUCCESS) throw std::runtime_error("Unable to connect MQTT client");
        mosquitto_loop_start(client_);
    }

    ~Monitor() {
        if (client_) { mosquitto_loop_stop(client_, true); mosquitto_destroy(client_); }
        mosquitto_lib_cleanup();
    }

private:
    static void on_connect(mosquitto* client, void* context, int result) {
        auto* monitor = static_cast<Monitor*>(context);
        if (result != MOSQ_ERR_SUCCESS) { std::cerr << "MQTT connection refused: " << result << "\n"; return; }
        mosquitto_subscribe(client, nullptr, monitor->config_.topic.c_str(), 0);
        std::cerr << "Subscribed to " << monitor->config_.topic << "\n";
    }

    static void on_message(mosquitto*, void* context, const mosquitto_message* message) {
        auto* monitor = static_cast<Monitor*>(context);
        if (message->payload && message->payloadlen > 0) monitor->database_.insert(message->topic, message->payload, message->payloadlen);
    }

    const Config& config_;
    Database& database_;
    mosquitto* client_ = nullptr;
};

constexpr const char* PAGE = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Meshtastic Monitor</title><style>body{margin:0;background:#f5f7f6;color:#17221e;font:14px Georgia,serif}main{max-width:1450px;margin:auto;padding:28px 18px}h1{font-size:28px;margin:0 0 5px}p{color:#52605a;margin:0 0 22px}table{width:100%;border-collapse:collapse;background:#fff}th,td{padding:10px;text-align:left;border-bottom:1px solid #dce4df;vertical-align:top}th{background:#e2ebe5;font-size:12px;letter-spacing:0}code{display:block;max-width:460px;white-space:pre-wrap;word-break:break-word;font:12px ui-monospace,monospace}.detail{min-width:220px}.detail strong{display:block;color:#234f3e;font-size:13px;margin-bottom:4px}.detail span{display:block;margin:2px 0}.meta{color:#65736c;font-size:12px}.raw{margin-top:7px}.raw summary{color:#65736c;cursor:pointer;font-size:12px}.muted{color:#87938d}@media(max-width:1050px){main{padding:18px 10px}th:nth-child(2),td:nth-child(2){display:none}}@media(max-width:760px){td,th{padding:7px}.topic,.topic-cell{display:none}.detail{min-width:150px}}@media(max-width:560px){th:nth-child(3),td:nth-child(3),th:nth-child(5),td:nth-child(5){display:none}}</style></head><body><main><h1>Meshtastic Monitor</h1><p id="status">Loading recent packets...</p><table><thead><tr><th>Received</th><th>Topic</th><th>Region</th><th>Channel</th><th>Node</th><th>Packet</th><th>Sender</th><th>Details</th></tr></thead><tbody id="packets"></tbody></table></main><script>const body=document.querySelector('#packets'),status=document.querySelector('#status');function esc(s){const x=document.createElement('span');x.textContent=s==null?'':String(s);return x.innerHTML}function number(v,unit){return v==null?'':`<span>${esc(v)}${unit||''}</span>`}function details(x){const m=x.measurement;if(!m)return '<span class="muted">No decoded measurement</span>';let html=`<strong>${esc(m.kind)}</strong>`;if(m.text)html+=`<span>${esc(m.text)}</span>`;if(m.long_name||m.short_name)html+=`<span>${esc(m.long_name)} ${m.short_name?'('+esc(m.short_name)+')':''}</span>`;if(m.latitude!=null)html+=number(m.latitude,', ')+number(m.longitude,'');if(m.altitude!=null)html+=number(m.altitude,' m');if(m.battery_level!=null)html+=number(m.battery_level,'% battery');if(m.voltage!=null)html+=number(m.voltage,' V');if(m.temperature!=null)html+=number(m.temperature,' C');if(m.relative_humidity!=null)html+=number(m.relative_humidity,'% RH');if(m.pressure!=null)html+=number(m.pressure,' hPa');return html}async function load(){try{const p=await (await fetch('api/packets')).json();body.innerHTML=p.map(x=>`<tr><td>${new Date(x.received_at*1000).toLocaleString()}</td><td class="topic-cell"><code>${esc(x.topic)}</code><span class="meta">${esc(x.transport)} / ${esc(x.encoding)}</span></td><td>${esc(x.region)}</td><td>${esc(x.channel)}</td><td>${esc(x.node)}</td><td><strong>${esc(x.packet_type)||'<span class="muted">binary</span>'}</strong><span class="meta">${esc(x.sender)}</span></td><td>${esc(x.sender)}</td><td class="detail">${details(x)}<details class="raw"><summary>Raw payload</summary><code>${esc(x.decoded_payload_hex||x.payload_hex)}</code></details></td></tr>`).join('');status.textContent=`${p.length} most recent packets`; }catch(e){status.textContent='Unable to load packets';}}load();setInterval(load,10000)</script></body></html>)HTML";

void send_response(int client, const char* type, const std::string& body) {
    const std::string header = "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(type) + "; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    send(client, header.data(), header.size(), MSG_NOSIGNAL);
    send(client, body.data(), body.size(), MSG_NOSIGNAL);
}

void serve_http(Database& database, int port) {
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) throw std::runtime_error("Unable to create HTTP socket");
    int enabled = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_ANY); address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(server, 8) < 0) throw std::runtime_error("Unable to listen on HTTP port");
    std::cerr << "HTTP dashboard on port " << port << "\n";
    while (running) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(server, &readable);
        timeval timeout{1, 0};
        if (select(server + 1, &readable, nullptr, nullptr, &timeout) <= 0) continue;
        const int client = accept(server, nullptr, nullptr);
        if (client < 0) continue;
        char request[1024]{};
        const ssize_t size = recv(client, request, sizeof(request) - 1, 0);
        const std::string line(request, size > 0 ? static_cast<size_t>(size) : 0);
        if (line.rfind("GET /api/packets ", 0) == 0) send_response(client, "application/json", database.recent_json());
        else send_response(client, "text/html", PAGE);
        close(client);
    }
    close(server);
}

Config parse_args(int argc, char** argv) {
    Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--help") {
            std::cout << "Usage: meshat-monitor [--database PATH] [--topic MQTT_TOPIC] [--retention-days DAYS] [--http-port PORT]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) throw std::runtime_error("Missing value for " + option);
        const std::string value = argv[++index];
        if (option == "--database") config.database = value;
        else if (option == "--retention-days") config.retention_days = std::stoi(value);
        else if (option == "--topic") config.topic = value;
        else if (option == "--http-port") config.http_port = std::stoi(value);
        else if (option == "--host") config.host = value;
        else if (option == "--mqtt-port") config.mqtt_port = std::stoi(value);
        else if (option == "--username") config.username = value;
        else if (option == "--password") config.password = value;
        else throw std::runtime_error("Unknown option: " + option);
    }
    if (config.retention_days < 1) throw std::runtime_error("--retention-days must be at least 1");
    return config;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Config config = parse_args(argc, argv);
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
        Database database(config.database);
        database.purge(config.retention_days);
        Monitor monitor(config, database);
        monitor.run();
        std::thread web([&] { serve_http(database, config.http_port); });
        auto next_purge = std::chrono::steady_clock::now() + std::chrono::hours(1);
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (std::chrono::steady_clock::now() >= next_purge) {
                database.purge(config.retention_days);
                next_purge += std::chrono::hours(1);
            }
        }
        web.join();
    } catch (const std::exception& error) {
        std::cerr << "meshat-monitor: " << error.what() << "\n";
        return 1;
    }
    return 0;
}