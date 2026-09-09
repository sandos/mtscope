#include "../src/packet.h"

#include <cstring>

#include <gtest/gtest.h>
#include "meshtastic/mqtt.pb.h"

#ifdef MTSCOPE_HAVE_OPENSSL
#include <openssl/evp.h>
#endif

namespace {

TEST(ParserTest, ParsesJsonTextAndTopicMetadata) {
    const std::string payload = R"({"type":"text","sender":"!abcd1234","text":"hello mesh"})";
    const ParsedPacket packet = parse_packet("msh/SE/2/json/LongFast/!abcd1234", payload.data(), static_cast<int>(payload.size()));

    EXPECT_EQ(packet.region, "SE");
    EXPECT_EQ(packet.transport, "2");
    EXPECT_EQ(packet.encoding, "json");
    EXPECT_EQ(packet.channel, "LongFast");
    EXPECT_EQ(packet.node, "!abcd1234");
    EXPECT_EQ(packet.packet_type, "text");
    EXPECT_EQ(packet.sender, "!abcd1234");
    ASSERT_TRUE(packet.measurement.has_value());
    EXPECT_EQ(packet.measurement->kind, "text");
    EXPECT_EQ(packet.measurement->text, "hello mesh");
}

TEST(ParserTest, ParsesJsonTelemetryMeasurements) {
    const std::string payload = R"({"type":"telemetry","sender":"!12345678","batteryLevel":87,"voltage":4.1,"temperature":21.5,"relativeHumidity":48.0,"barometricPressure":1012.3})";
    const ParsedPacket packet = parse_packet("msh/NO/2/json/LongFast", payload.data(), static_cast<int>(payload.size()));

    ASSERT_TRUE(packet.measurement.has_value());
    EXPECT_EQ(packet.measurement->kind, "telemetry");
    EXPECT_DOUBLE_EQ(*packet.measurement->battery_level, 87.0);
    EXPECT_DOUBLE_EQ(*packet.measurement->voltage, 4.1);
    EXPECT_DOUBLE_EQ(*packet.measurement->temperature, 21.5);
    EXPECT_DOUBLE_EQ(*packet.measurement->relative_humidity, 48.0);
    EXPECT_DOUBLE_EQ(*packet.measurement->pressure, 1012.3);
}

TEST(ParserTest, ParsesDecodedProtobufText) {
    meshtastic::ServiceEnvelope envelope;
    envelope.set_gateway_id("!observer01");
    auto* mesh_packet = envelope.mutable_packet();
    mesh_packet->set_from(0x12345678);
    mesh_packet->set_id(0x01020304);
    auto* data = mesh_packet->mutable_decoded();
    data->set_portnum(meshtastic::TEXT_MESSAGE_APP);
    data->set_payload("protobuf hello");

    const std::string payload = envelope.SerializeAsString();
    const ParsedPacket packet = parse_packet("msh/SE/2/e/LongFast", payload.data(), static_cast<int>(payload.size()));

    EXPECT_EQ(packet.sender, "!12345678");
    EXPECT_EQ(packet.observer, "!observer01");
    EXPECT_FALSE(packet.content_hash.empty());
    EXPECT_EQ(packet.packet_type, "TEXT_MESSAGE_APP");
    ASSERT_TRUE(packet.measurement.has_value());
    EXPECT_EQ(packet.measurement->kind, "text");
    EXPECT_EQ(packet.measurement->text, "protobuf hello");
}

TEST(ParserTest, HashesInnerPacketIndependentlyOfObserver) {
    meshtastic::ServiceEnvelope first;
    first.set_gateway_id("!observer01");
    first.mutable_packet()->set_from(0x12345678);
    first.mutable_packet()->set_id(0x01020304);
    first.mutable_packet()->set_to(0x87654321);

    meshtastic::ServiceEnvelope second = first;
    second.set_gateway_id("!observer02");

    const std::string first_payload = first.SerializeAsString();
    const std::string second_payload = second.SerializeAsString();
    const ParsedPacket first_packet = parse_packet("msh/SE/2/e/LongFast", first_payload.data(), static_cast<int>(first_payload.size()));
    const ParsedPacket second_packet = parse_packet("msh/SE/2/e/LongFast", second_payload.data(), static_cast<int>(second_payload.size()));

    EXPECT_NE(first_packet.observer, second_packet.observer);
    EXPECT_EQ(first_packet.content_hash, second_packet.content_hash);
}

#ifdef MTSCOPE_HAVE_OPENSSL
TEST(ParserTest, DecryptsDefaultChannelText) {
    static constexpr unsigned char key[] = {
        0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
        0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
    };
    const std::uint32_t sender = 0x12345678;
    const std::uint64_t packet_id = 0x01020304;

    meshtastic::Data data;
    data.set_portnum(meshtastic::TEXT_MESSAGE_APP);
    data.set_payload("encrypted hello");
    const std::string plaintext = data.SerializeAsString();

    unsigned char nonce[16] = {};
    std::memcpy(nonce, &packet_id, sizeof(packet_id));
    std::memcpy(nonce + sizeof(packet_id), &sender, sizeof(sender));
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    ASSERT_NE(context, nullptr);
    ASSERT_EQ(EVP_EncryptInit_ex(context, EVP_aes_128_ctr(), nullptr, key, nonce), 1);
    std::string ciphertext(plaintext.size(), '\0');
    int ciphertext_length = 0;
    ASSERT_EQ(EVP_EncryptUpdate(context, reinterpret_cast<unsigned char*>(ciphertext.data()), &ciphertext_length,
        reinterpret_cast<const unsigned char*>(plaintext.data()), static_cast<int>(plaintext.size())), 1);
    int final_length = 0;
    ASSERT_EQ(EVP_EncryptFinal_ex(context, reinterpret_cast<unsigned char*>(ciphertext.data()) + ciphertext_length, &final_length), 1);
    EVP_CIPHER_CTX_free(context);
    ciphertext.resize(static_cast<size_t>(ciphertext_length + final_length));

    meshtastic::MeshPacket mesh_packet;
    mesh_packet.set_from(sender);
    mesh_packet.set_id(packet_id);
    mesh_packet.set_encrypted(ciphertext);
    std::string decrypted;
    ASSERT_TRUE(decrypt_default_channel(mesh_packet, decrypted));
    EXPECT_EQ(decrypted, plaintext);
}
#endif

}  // namespace
