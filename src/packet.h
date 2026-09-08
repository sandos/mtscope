#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace meshtastic {
class MeshPacket;
}

struct Measurement {
    std::string kind;
    std::string text;
    std::string node_id;
    std::string long_name;
    std::string short_name;
    std::string hardware_model;
    std::string role;
    std::optional<bool> is_licensed;
    std::optional<bool> is_unmessagable;
    bool has_public_key = false;
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

std::string hex_encode(const void* data, int length);
ParsedPacket parse_packet(const std::string& topic, const void* payload, int length);

#ifdef MTSCOPE_HAVE_OPENSSL
bool decrypt_default_channel(const meshtastic::MeshPacket& mesh_packet, std::string& plaintext);
#endif