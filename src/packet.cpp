#include "packet.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "meshtastic/mqtt.pb.h"
#ifdef MTSCOPE_HAVE_OPENSSL
#include <openssl/evp.h>
#endif

namespace {

std::string node_id(std::uint32_t value) {
    char buffer[10];
    std::snprintf(buffer, sizeof(buffer), "!%08x", value);
    return buffer;
}

std::string content_hash(const std::string& value) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    char output[17];
    std::snprintf(output, sizeof(output), "%016llx", static_cast<unsigned long long>(hash));
    return output;
}

std::string packet_key(const meshtastic::MeshPacket& mesh_packet) {
    std::string identity;
    const auto append_value = [&identity](std::uint32_t value) {
        identity.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    append_value(mesh_packet.from());
    append_value(mesh_packet.to());
    append_value(mesh_packet.id());
    append_value(mesh_packet.channel());
    if (mesh_packet.has_decoded()) identity += mesh_packet.decoded().SerializeAsString();
    if (mesh_packet.has_encrypted()) identity += mesh_packet.encrypted();
    return content_hash(identity);
}

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

struct JsonValue {
    enum class Type { String, Number, Boolean, Null, Object, Array };
    Type type;
    std::string text;
    std::map<std::string, JsonValue> object;
};

void append_codepoint(std::string& output, unsigned int codepoint) {
    if (codepoint <= 0x7f) {
        output += static_cast<char>(codepoint);
    } else if (codepoint <= 0x7ff) {
        output += static_cast<char>(0xc0 | (codepoint >> 6));
        output += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint <= 0xffff) {
        output += static_cast<char>(0xe0 | (codepoint >> 12));
        output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        output += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else {
        output += static_cast<char>(0xf0 | (codepoint >> 18));
        output += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        output += static_cast<char>(0x80 | (codepoint & 0x3f));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    std::optional<JsonValue> parse() {
        try {
            skip_space();
            JsonValue value = parse_value();
            skip_space();
            if (position_ != input_.size()) throw std::runtime_error("trailing JSON");
            return value;
        } catch (const std::runtime_error&) {
            return std::nullopt;
        }
    }

private:
    static bool is_digit(char character) { return character >= '0' && character <= '9'; }

    char consume() {
        if (position_ >= input_.size()) throw std::runtime_error("unexpected end");
        return input_[position_++];
    }

    void expect(char expected) {
        if (consume() != expected) throw std::runtime_error("unexpected character");
    }

    void skip_space() {
        while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\t' ||
            input_[position_] == '\r' || input_[position_] == '\n')) ++position_;
    }

    unsigned int parse_hex() {
        unsigned int value = 0;
        for (int index = 0; index < 4; ++index) {
            const char character = consume();
            value <<= 4;
            if (character >= '0' && character <= '9') value += static_cast<unsigned int>(character - '0');
            else if (character >= 'a' && character <= 'f') value += static_cast<unsigned int>(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F') value += static_cast<unsigned int>(character - 'A' + 10);
            else throw std::runtime_error("invalid Unicode escape");
        }
        return value;
    }

    std::string parse_string() {
        expect('"');
        std::string output;
        while (position_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(consume());
            if (character == '"') return output;
            if (character < 0x20) throw std::runtime_error("control character in string");
            if (character != '\\') {
                output += static_cast<char>(character);
                continue;
            }
            switch (consume()) {
            case '"': output += '"'; break;
            case '\\': output += '\\'; break;
            case '/': output += '/'; break;
            case 'b': output += '\b'; break;
            case 'f': output += '\f'; break;
            case 'n': output += '\n'; break;
            case 'r': output += '\r'; break;
            case 't': output += '\t'; break;
            case 'u': {
                const unsigned int high = parse_hex();
                if (high >= 0xd800 && high <= 0xdbff) {
                    expect('\\');
                    expect('u');
                    const unsigned int low = parse_hex();
                    if (low < 0xdc00 || low > 0xdfff) throw std::runtime_error("invalid surrogate pair");
                    append_codepoint(output, 0x10000 + ((high - 0xd800) << 10) + (low - 0xdc00));
                } else if (high >= 0xdc00 && high <= 0xdfff) {
                    throw std::runtime_error("unexpected low surrogate");
                } else {
                    append_codepoint(output, high);
                }
                break;
            }
            default: throw std::runtime_error("invalid string escape");
            }
        }
        throw std::runtime_error("unterminated string");
    }

    JsonValue parse_number() {
        const size_t start = position_;
        if (input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) throw std::runtime_error("invalid number");
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && is_digit(input_[position_])) throw std::runtime_error("leading zero");
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') throw std::runtime_error("invalid number");
            while (position_ < input_.size() && is_digit(input_[position_])) ++position_;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            if (position_ >= input_.size() || !is_digit(input_[position_])) throw std::runtime_error("invalid fraction");
            while (position_ < input_.size() && is_digit(input_[position_])) ++position_;
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            if (position_ >= input_.size() || !is_digit(input_[position_])) throw std::runtime_error("invalid exponent");
            while (position_ < input_.size() && is_digit(input_[position_])) ++position_;
        }
        return {JsonValue::Type::Number, std::string(input_.substr(start, position_ - start)), {}};
    }

    JsonValue parse_value() {
        skip_space();
        if (position_ >= input_.size()) throw std::runtime_error("missing value");
        switch (input_[position_]) {
        case '"': return {JsonValue::Type::String, parse_string(), {}};
        case '{': return parse_object();
        case '[': return parse_array();
        case 't': return parse_literal("true", JsonValue::Type::Boolean);
        case 'f': return parse_literal("false", JsonValue::Type::Boolean);
        case 'n': return parse_literal("null", JsonValue::Type::Null);
        default: return parse_number();
        }
    }

    JsonValue parse_literal(std::string_view literal, JsonValue::Type type) {
        if (input_.substr(position_, literal.size()) != literal) throw std::runtime_error("invalid literal");
        position_ += literal.size();
        return {type, std::string(literal), {}};
    }

    JsonValue parse_object() {
        expect('{');
        JsonValue value{JsonValue::Type::Object, {}, {}};
        skip_space();
        if (position_ < input_.size() && input_[position_] == '}') {
            ++position_;
            return value;
        }
        while (true) {
            skip_space();
            const std::string key = parse_string();
            skip_space();
            expect(':');
            JsonValue member = parse_value();
            if (!value.object.emplace(key, std::move(member)).second) throw std::runtime_error("duplicate key");
            skip_space();
            const char separator = consume();
            if (separator == '}') return value;
            if (separator != ',') throw std::runtime_error("invalid object");
        }
    }

    JsonValue parse_array() {
        expect('[');
        skip_space();
        if (position_ < input_.size() && input_[position_] == ']') {
            ++position_;
            return {JsonValue::Type::Array, {}, {}};
        }
        while (true) {
            parse_value();
            skip_space();
            const char separator = consume();
            if (separator == ']') return {JsonValue::Type::Array, {}, {}};
            if (separator != ',') throw std::runtime_error("invalid array");
        }
    }

    std::string_view input_;
    size_t position_ = 0;
};

std::string json_field(const std::string& json, const std::string& name) {
    const auto document = JsonParser(json).parse();
    if (!document || document->type != JsonValue::Type::Object) return {};
    const auto field = document->object.find(name);
    if (field == document->object.end()) return {};
    if (field->second.type == JsonValue::Type::String || field->second.type == JsonValue::Type::Number ||
        field->second.type == JsonValue::Type::Boolean) return field->second.text;
    return {};
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
    } else if (data.portnum() == meshtastic::MAP_REPORT_APP) {
        meshtastic::MapReport map_report;
        if (!map_report.ParseFromString(data_payload)) return false;
        Measurement measurement;
        measurement.kind = "map_report";
        measurement.node_id = packet.sender;
        measurement.long_name = map_report.long_name();
        measurement.short_name = map_report.short_name();
        measurement.hardware_model = meshtastic::HardwareModel_Name(map_report.hw_model());
        measurement.role = meshtastic::Config_DeviceConfig_Role_Name(map_report.role());
        if (map_report.has_opted_report_location()) {
            measurement.latitude = map_report.latitude_i() * 1e-7;
            measurement.longitude = map_report.longitude_i() * 1e-7;
            measurement.altitude = map_report.altitude();
        }
        packet.measurement = measurement;
    } else if (data.portnum() == meshtastic::NODEINFO_APP) {
        meshtastic::User user;
        if (!user.ParseFromString(data_payload)) return false;
        Measurement measurement;
        measurement.kind = "nodeinfo";
        measurement.node_id = user.id().empty() ? packet.sender : user.id();
        measurement.long_name = user.long_name();
        measurement.short_name = user.short_name();
        measurement.hardware_model = meshtastic::HardwareModel_Name(user.hw_model());
        measurement.role = meshtastic::Config_DeviceConfig_Role_Name(user.role());
        measurement.is_licensed = user.is_licensed();
        if (user.has_is_unmessagable()) measurement.is_unmessagable = user.is_unmessagable();
        measurement.has_public_key = !user.public_key().empty();
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

} // namespace

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
        packet.observer = json_field(json, "gatewayId");
        if (packet.observer.empty()) packet.observer = json_field(json, "gateway_id");
        packet.packet_key = content_hash(json);
        packet.logical_payload = json;
        if (packet.channel.empty()) packet.channel = json_field(json, "channel");
        Measurement measurement;
        if (packet.packet_type == "text") {
            measurement.kind = "text";
            measurement.text = json_field(json, "text");
        } else if (packet.packet_type == "telemetry" || packet.packet_type.find("Telemetry") != std::string::npos) {
            measurement.kind = "telemetry";
            measurement.battery_level = json_number(json, "batteryLevel");
            measurement.voltage = json_number(json, "voltage");
            measurement.temperature = json_number(json, "temperature");
            measurement.relative_humidity = json_number(json, "relativeHumidity");
            measurement.pressure = json_number(json, "barometricPressure");
        } else if (packet.packet_type == "position") {
            measurement.kind = "position";
            measurement.latitude = json_number(json, "latitude");
            measurement.longitude = json_number(json, "longitude");
            measurement.altitude = json_number(json, "altitude");
        } else if (packet.packet_type == "nodeinfo") {
            measurement.kind = "nodeinfo";
            measurement.long_name = json_field(json, "longName");
            measurement.short_name = json_field(json, "shortName");
        }
        if (!measurement.kind.empty()) {
            measurement.node_id = packet.sender;
            packet.measurement = measurement;
        }
    } else if (packet.encoding == "stat") {
        packet.packet_type = "status";
        packet.sender = packet.node;
        packet.packet_key = content_hash(std::string(static_cast<const char*>(payload), static_cast<size_t>(length)));
        packet.logical_payload.assign(static_cast<const char*>(payload), static_cast<size_t>(length));
    } else {
        meshtastic::ServiceEnvelope envelope;
        if (envelope.ParseFromArray(payload, length) && envelope.has_packet()) {
            const auto& mesh_packet = envelope.packet();
            packet.sender = node_id(mesh_packet.from());
            packet.destination = node_id(mesh_packet.to());
            packet.observer = envelope.gateway_id();
            packet.packet_key = packet_key(mesh_packet);
            if (mesh_packet.has_decoded()) packet.logical_payload = mesh_packet.decoded().SerializeAsString();
            else if (mesh_packet.has_encrypted()) packet.logical_payload = mesh_packet.encrypted();
            packet.mesh_packet_id = mesh_packet.id();
            if (mesh_packet.has_rx_time()) packet.rx_time = mesh_packet.rx_time();
            packet.rx_snr = mesh_packet.rx_snr();
            if (mesh_packet.has_rx_rssi()) packet.rx_rssi = mesh_packet.rx_rssi();
            packet.hop_limit = mesh_packet.hop_limit();
            packet.hop_start = mesh_packet.hop_start();
            packet.via_mqtt = mesh_packet.via_mqtt();
            if (mesh_packet.has_decoded()) {
                decode_data(packet, mesh_packet.decoded());
            } else if (mesh_packet.has_encrypted()) {
                packet.packet_type = "encrypted";
#ifdef MTSCOPE_HAVE_OPENSSL
                std::string plaintext;
                meshtastic::Data data;
                if (decrypt_default_channel(mesh_packet, plaintext) && data.ParseFromString(plaintext) && decode_data(packet, data)) {
                    packet.packet_type = "decrypted_" + packet.packet_type;
                }
#endif
            }
        }
    }
    if (packet.packet_key.empty()) {
        const std::string raw(static_cast<const char*>(payload), static_cast<size_t>(length));
        packet.packet_key = content_hash(raw);
        packet.logical_payload = raw;
    }
    return packet;
}