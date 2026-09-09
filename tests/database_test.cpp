#include "../src/database.h"

#include <string>

#include <gtest/gtest.h>

TEST(DatabaseTest, PersistsReceivedPacketAndMeasurement) {
    Database database(":memory:");
    const std::string payload = R"({"type":"text","sender":"!abcd1234","text":"hello mesh"})";

    ASSERT_TRUE(database.insert("msh/SE/2/json/LongFast/!abcd1234", payload.data(), static_cast<int>(payload.size())));

    const std::string recent = database.recent_json();
    EXPECT_NE(recent.find("!abcd1234"), std::string::npos);
    EXPECT_NE(recent.find("hello mesh"), std::string::npos);
    EXPECT_NE(recent.find("\"observation_count\":1"), std::string::npos);
}

TEST(DatabaseTest, DoesNotGroupRepeatedPacketsWithoutMeshIds) {
    Database database(":memory:");
    const std::string payload = "online";

    ASSERT_TRUE(database.insert("msh/SE/2/stat/!abcd1234", payload.data(), static_cast<int>(payload.size())));
    ASSERT_TRUE(database.insert("msh/SE/2/stat/!abcd1234", payload.data(), static_cast<int>(payload.size())));

    const std::string recent = database.recent_json();
    EXPECT_EQ(recent.find("\"observation_count\":2"), std::string::npos);
    EXPECT_NE(recent.find("\"observation_count\":1"), std::string::npos);
}