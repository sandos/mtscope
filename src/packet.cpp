#include "packet.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
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
        packet.content_hash = content_hash(json);
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
    } else {
        meshtastic::ServiceEnvelope envelope;
        if (envelope.ParseFromArray(payload, length) && envelope.has_packet()) {
            const auto& mesh_packet = envelope.packet();
            packet.sender = node_id(mesh_packet.from());
            packet.observer = envelope.gateway_id();
            packet.content_hash = content_hash(mesh_packet.SerializeAsString());
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
    return packet;
}