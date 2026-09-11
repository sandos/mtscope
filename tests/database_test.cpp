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
    EXPECT_EQ(recent, "[]");
}

TEST(DatabaseTest, OnlineStatusKeepsKnownNodeVisible) {
    Database database(":memory:");
    const std::string nodeinfo = R"({"type":"nodeinfo","sender":"!abcd1234","longName":"Mesh node","shortName":"MN"})";
    const std::string online = "online";

    ASSERT_TRUE(database.insert("msh/SE/2/json/LongFast/!abcd1234", nodeinfo.data(), static_cast<int>(nodeinfo.size())));
    ASSERT_TRUE(database.insert("msh/SE/2/stat/!abcd1234", online.data(), static_cast<int>(online.size())));

    EXPECT_NE(database.nodes_json().find("!abcd1234"), std::string::npos);
}

TEST(DatabaseTest, ReportsDatabaseStatistics) {
    Database database(":memory:");
    const std::string payload = R"({"type":"nodeinfo","sender":"!abcd1234","longName":"Mesh node","shortName":"MN"})";

    ASSERT_TRUE(database.insert("msh/SE/2/json/LongFast/!abcd1234", payload.data(), static_cast<int>(payload.size())));

    const std::string stats = database.stats_json();
    EXPECT_NE(stats.find("\"database_bytes\":"), std::string::npos);
    EXPECT_NE(stats.find("\"logical_packets\":1"), std::string::npos);
    EXPECT_NE(stats.find("\"observations\":1"), std::string::npos);
    EXPECT_NE(stats.find("\"measurements\":1"), std::string::npos);
    EXPECT_NE(stats.find("\"known_nodes\":1"), std::string::npos);
}

TEST(DatabaseTest, PurgesExpiredObservationsAndDependentData) {
    Database database(":memory:");
    const std::string payload = R"({"type":"nodeinfo","sender":"!abcd1234","longName":"Mesh node","shortName":"MN"})";

    ASSERT_TRUE(database.insert("msh/SE/2/json/LongFast/!abcd1234", payload.data(), static_cast<int>(payload.size())));
    EXPECT_NE(database.recent_json(), "[]");
    EXPECT_NE(database.nodes_json(), "[]");

    database.purge(-1);

    EXPECT_EQ(database.recent_json(), "[]");
    EXPECT_EQ(database.nodes_json(), "[]");
}