#include <HalStorage.h>
#include <gtest/gtest.h>

#include "AnkiConnectionStore.h"

namespace {
constexpr char PATH[] = "/.crossink-anki/connection.json";
constexpr char BACKUP[] = "/.crossink-anki/connection.json.bak";

class ConnectionStoreTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage = {}; }
  void put(const char* path, const std::string& json) { Storage.files[path] = {json.begin(), json.end()}; }
};

TEST_F(ConnectionStoreTest, RoundTripsConfiguredAndPartiallyEnteredConnection) {
  for (const anki::Connection& expected :
       {anki::Connection{"http://desktop:5050", "secret"}, anki::Connection{"http://desktop:5050", ""},
        anki::Connection{"", "secret"}, anki::Connection{}}) {
    ASSERT_TRUE(anki::AnkiConnectionStore::save(expected));
    anki::Connection actual;
    Storage.openCalls = 0;
    ASSERT_TRUE(anki::AnkiConnectionStore::load(actual));
    EXPECT_EQ(Storage.openCalls, 1U);
    EXPECT_EQ(actual.url, expected.url);
    EXPECT_EQ(actual.token, expected.token);
    EXPECT_EQ(actual.configured(), !expected.url.empty() && !expected.token.empty());
  }
}

TEST_F(ConnectionStoreTest, AcceptsOnlyBoundedOriginsAndHeaderSafeTokens) {
  for (const char* address : {"http://desktop", "https://desktop:5050", "http://192.168.1.2:5050/"}) {
    std::string url = address;
    EXPECT_TRUE(anki::AnkiConnectionStore::normalizeUrl(url)) << address;
    EXPECT_FALSE(url.ends_with('/'));
  }
  for (const char* address :
       {"http://", "desktop:5050", "ftp://desktop", "http://user@desktop", "http://desktop:0", "http://desktop:65536",
        "http://desktop/cards", "http://desktop?x=y", "http://desktop#fragment", "http://desktop\r\nHeader:evil"}) {
    std::string url = address;
    EXPECT_FALSE(anki::AnkiConnectionStore::normalizeUrl(url)) << address;
  }
  EXPECT_TRUE(anki::AnkiConnectionStore::validToken(std::string(128, 'x')));
  EXPECT_FALSE(anki::AnkiConnectionStore::validToken(std::string(129, 'x')));
  EXPECT_FALSE(anki::AnkiConnectionStore::validToken("secret\r\nHeader:evil"));
  EXPECT_FALSE(anki::AnkiConnectionStore::validToken("secret with spaces"));
}

TEST_F(ConnectionStoreTest, MissingConfigurationIsExpectedAndClearsPriorOutput) {
  anki::Connection connection{"http://stale", "stale"};
  ASSERT_TRUE(anki::AnkiConnectionStore::load(connection));
  EXPECT_FALSE(connection.configured());
  EXPECT_TRUE(connection.url.empty());
  EXPECT_TRUE(connection.token.empty());
}

TEST_F(ConnectionStoreTest, InvalidJsonFieldsNeverBecomeAnEmptyOrTruncatedConnection) {
  for (const char* json : {R"({"version":1})", R"({"version":1,"url":42,"token_obf":""})",
                           R"({"version":1,"url":"http://desktop","token_obf":false})",
                           R"({"version":"1","url":"http://desktop","token_obf":""})",
                           R"({"version":1,"url":"http://desktop\u0000evil","token_obf":""})",
                           R"({"version":1,"url":"http://desktop","token_obf":"encoded:secret\u0000evil"})",
                           R"({"version":1,"url":"http://desktop","token_obf":"corrupt"})"}) {
    put(PATH, json);
    anki::Connection connection{"http://stale", "stale"};
    EXPECT_FALSE(anki::AnkiConnectionStore::load(connection)) << json;
    EXPECT_TRUE(connection.url.empty());
    EXPECT_TRUE(connection.token.empty());
  }
}

TEST_F(ConnectionStoreTest, RejectsTrailingDataAndLiteralNullsInConfiguration) {
  const std::string valid = R"({"version":1,"url":"http://desktop","token_obf":"encoded:secret"})";
  for (const auto& suffix : {std::string(" trailing"), std::string("{}"), std::string("\0hidden", 7)}) {
    put(PATH, valid + suffix);
    anki::Connection connection;
    EXPECT_FALSE(anki::AnkiConnectionStore::load(connection));
    EXPECT_FALSE(connection.configured());
  }
}

TEST_F(ConnectionStoreTest, ReadAndCloseFailuresLeaveEmptyOutputAndNoOpenHandle) {
  const std::string valid = R"({"version":1,"url":"http://desktop","token_obf":"encoded:secret"})";
  for (int failAfter = 0; failAfter < 3; ++failAfter) {
    SCOPED_TRACE(failAfter);
    Storage = {};
    put(PATH, valid);
    Storage.failAfter = failAfter;
    anki::Connection connection{"http://stale", "stale"};
    EXPECT_FALSE(anki::AnkiConnectionStore::load(connection));
    EXPECT_TRUE(connection.url.empty());
    EXPECT_TRUE(connection.token.empty());
    EXPECT_EQ(Storage.handles[PATH], 0);
  }
}

TEST_F(ConnectionStoreTest, ReadsBackupDirectlyWhenRecoveryRenameFails) {
  put(BACKUP, R"({"version":1,"url":"http://desktop","token_obf":"encoded:secret"})");
  Storage.failAfter = 3;  // Open, read and close succeed; recovery rename fails.
  anki::Connection connection;
  ASSERT_TRUE(anki::AnkiConnectionStore::load(connection));
  EXPECT_TRUE(connection.configured());
  EXPECT_TRUE(Storage.exists(BACKUP));
  EXPECT_FALSE(Storage.exists(PATH));
}

TEST_F(ConnectionStoreTest, BoundsBothCurrentAndRecoveryFilesBeforeParsing) {
  for (const char* path : {PATH, BACKUP}) {
    Storage = {};
    put(path, std::string(2049, ' '));
    anki::Connection connection;
    EXPECT_FALSE(anki::AnkiConnectionStore::load(connection));
    EXPECT_TRUE(Storage.exists(path));
  }
}

TEST_F(ConnectionStoreTest, RecoversAnInterruptedSettingsReplacementFromBackup) {
  ASSERT_TRUE(anki::AnkiConnectionStore::save({"http://desktop:5050", "secret"}));
  ASSERT_TRUE(Storage.rename(PATH, BACKUP));
  anki::Connection connection;
  Storage.openCalls = 0;
  ASSERT_TRUE(anki::AnkiConnectionStore::load(connection));
  EXPECT_EQ(Storage.openCalls, 1U);
  EXPECT_EQ(connection.url, "http://desktop:5050");
  EXPECT_EQ(connection.token, "secret");
  EXPECT_TRUE(Storage.exists(PATH));
}

TEST_F(ConnectionStoreTest, FailedReplacementKeepsPreviousConnectionRecoverable) {
  ASSERT_TRUE(anki::AnkiConnectionStore::save({"http://old:5050", "old"}));
  const auto files = Storage.files;
  for (int failAfter = 0; failAfter < 15; ++failAfter) {
    SCOPED_TRACE(failAfter);
    Storage.files = files;
    Storage.failAfter = failAfter;
    anki::AnkiConnectionStore::save({"http://new:5050", "new"});
    Storage.failAfter = -1;
    anki::Connection connection;
    ASSERT_TRUE(anki::AnkiConnectionStore::load(connection));
    EXPECT_TRUE((connection.url == "http://old:5050" && connection.token == "old") ||
                (connection.url == "http://new:5050" && connection.token == "new"));
  }
}
}  // namespace
