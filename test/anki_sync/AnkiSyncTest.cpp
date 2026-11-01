#include <gtest/gtest.h>

#include <deque>
#include <string>
#include <vector>

#include "AnkiStore.h"
#include "AnkiSyncClient.h"
#include "network/HttpDownloader.h"

namespace {
struct Reply {
  std::string path;
  std::string body;
  HttpDownloader::DownloadError result = HttpDownloader::OK;
  bool failReceiptSync = false;
};
std::deque<Reply> replies;
std::vector<std::string> calls;
std::vector<std::string> batches;
std::vector<size_t> batchSizes;
std::vector<int64_t> uploadedCards;

constexpr char HEALTH[] = R"({"status":"ok","protocol_version":3,"collection_ready":true,"server_time":1790000000})";
const anki::Connection CONNECTION{"http://desktop:5050", "secret"};

std::string snapshot(const char* generation, const unsigned count = 1) {
  std::string output = "{\"type\":\"meta\",\"status\":\"success\",\"protocol_version\":3,\"pull_id\":\"" +
                       std::string(generation) + "\",\"card_count\":" + std::to_string(count) + "}\n";
  for (unsigned i = 0; i < count; ++i)
    output += "{\"type\":\"card\",\"id\":\"" + std::to_string(100 + i) +
              "\",\"deck_id\":\"1\",\"deck_name\":\"Deck\",\"front\":\"Question\",\"back\":\"Answer\"}\n";
  return output + "{\"type\":\"end\",\"card_count\":" + std::to_string(count) + "}\n";
}

class SyncTest : public ::testing::Test {
 protected:
  anki::AnkiStore store;
  anki::AnkiSyncClient client;
  void SetUp() override {
    Storage = {};
    replies.clear();
    calls.clear();
    batches.clear();
    batchSizes.clear();
    uploadedCards.clear();
    ASSERT_TRUE(store.open());
    ASSERT_TRUE(client.initialize(store));
  }
  void install(const unsigned count = 1) {
    ASSERT_TRUE(store.beginSnapshot("old", count));
    for (unsigned i = 0; i < count; ++i) {
      anki::CardInput card{int64_t(100 + i), 1, "Deck", "Q", "A", 1, 1};
      ASSERT_TRUE(store.appendCard(card));
    }
    ASSERT_TRUE(store.commitSnapshot());
  }
  void queueReview() {
    install();
    ASSERT_TRUE(store.recordReview(0, 3, 1234));
  }
};
}  // namespace

HttpDownloader::DownloadError HttpDownloader::requestToken(const std::string& url, const char* token,
                                                           const char* method, const std::string_view body,
                                                           const DataCallback& onData, const size_t maxResponseBytes,
                                                           int& status, CancelCallback shouldCancel) {
  calls.push_back(url);
  EXPECT_STREQ(token, url.ends_with("/health") ? "" : "secret");
  if (replies.empty()) {
    ADD_FAILURE() << "Unexpected HTTP request " << url;
    return HTTP_ERROR;
  }
  Reply reply = std::move(replies.front());
  replies.pop_front();
  EXPECT_EQ(url, CONNECTION.url + reply.path);
  EXPECT_STREQ(method, reply.path == "/push" ? "POST" : "GET");
  if (reply.path == "/push") {
    // The durable uncertainty marker must exist before the first request byte.
    anki::AnkiStore restarted;
    EXPECT_TRUE(restarted.open());
    EXPECT_TRUE(restarted.needsAttention());
    JsonDocument payload;
    EXPECT_FALSE(deserializeJson(payload, body.data(), body.size()));
    const std::string id = payload["batch_id"] | "";
    batches.push_back(id);
    batchSizes.push_back(payload["reviews"].size());
    for (const auto review : payload["reviews"].as<JsonArray>())
      uploadedCards.push_back(review["card_id"].as<int64_t>());
    if (reply.body == "success" || reply.body == "partial" || reply.body == "duplicate") {
      const auto count = payload["reviews"].size();
      const auto processed = reply.body == "success" ? count : reply.body == "partial" ? count - 1 : 0;
      const std::string rejected = reply.body == "partial" ? "[{\"index\":" + std::to_string(count - 1) +
                                                                 ",\"card_id\":\"100\",\"error\":\"missing\"}]"
                                                           : "[]";
      reply.body = "{\"status\":\"" + reply.body + "\",\"protocol_version\":3,\"batch_id\":\"" + id +
                   "\",\"processed\":" + std::to_string(processed) + ",\"rejected\":" + rejected + "}";
    }
  }
  if (shouldCancel && shouldCancel()) return ABORTED;
  status = 200;
  EXPECT_LE(reply.body.size(), maxResponseBytes);
  for (size_t offset = 0; offset < reply.body.size(); offset += 17) {
    const size_t count = std::min(size_t{17}, reply.body.size() - offset);
    if (!onData(reinterpret_cast<const uint8_t*>(reply.body.data() + offset), count)) return FILE_ERROR;
  }
  if (reply.failReceiptSync) Storage.failSync = true;
  return reply.result;
}

namespace {
TEST_F(SyncTest, DownloadsAnInitialSnapshotWithoutUploading) {
  replies = {{"/health", HEALTH}, {"/pull?max_cards=250&max_total=1000", snapshot("new", 2)}};
  EXPECT_EQ(client.run(CONNECTION), anki::SyncResult::Success);
  EXPECT_TRUE(replies.empty());
  EXPECT_STREQ(store.generation(), "new");
  EXPECT_EQ(store.cardCount(), 2);
}

TEST_F(SyncTest, AcknowledgesExactBatchBeforeInstallingReplacement) {
  queueReview();
  replies = {{"/health", HEALTH}, {"/push", "success"}, {"/pull?max_cards=250&max_total=1000", snapshot("new", 0)}};
  EXPECT_EQ(client.run(CONNECTION), anki::SyncResult::Success);
  EXPECT_TRUE(replies.empty());
  EXPECT_EQ(store.pendingCount(), 0);
  EXPECT_FALSE(store.needsAttention());
  EXPECT_STREQ(store.generation(), "new");
}

TEST_F(SyncTest, UploadsOrderedReviewsInDistinctBatchesOfAtMostThirtyTwo) {
  install(40);
  for (uint16_t i = 0; i < 40; ++i) ASSERT_TRUE(store.recordReview(i, 3, 100 + i));
  replies = {{"/health", HEALTH},
             {"/push", "success"},
             {"/push", "success"},
             {"/pull?max_cards=250&max_total=1000", snapshot("new", 0)}};
  EXPECT_EQ(client.run(CONNECTION), anki::SyncResult::Success);
  EXPECT_EQ(batchSizes, (std::vector<size_t>{32, 8}));
  ASSERT_EQ(batches.size(), size_t{2});
  EXPECT_NE(batches[0], batches[1]);
  ASSERT_EQ(uploadedCards.size(), size_t{40});
  for (int64_t i = 0; i < 40; ++i) EXPECT_EQ(uploadedCards[i], 100 + i);
  EXPECT_EQ(store.pendingCount(), 0);
}

TEST_F(SyncTest, PartiallyAppliedBatchPreservesEveryReviewForManualReconciliation) {
  install(2);
  ASSERT_TRUE(store.recordReview(0, 1, 100));
  ASSERT_TRUE(store.recordReview(1, 4, 200));
  replies = {{"/health", HEALTH}, {"/push", "partial"}};
  EXPECT_EQ(client.run(CONNECTION), anki::SyncResult::Attention);
  EXPECT_TRUE(store.needsAttention());
  EXPECT_EQ(store.pendingCount(), 2);
  EXPECT_TRUE(replies.empty());
}

TEST_F(SyncTest, RepeatedPassesReachDesktopInOrderAcrossBatchBoundaries) {
  install(20);
  for (int pass = 0; pass < 2; ++pass) {
    if (pass) {
      ASSERT_TRUE(store.restartDeck(1));
    }
    for (uint16_t i = 0; i < 20; ++i) ASSERT_TRUE(store.recordReview(i, pass + 2, 100 + i));
  }
  ASSERT_TRUE(store.open());
  replies = {{"/health", HEALTH},
             {"/push", "success"},
             {"/push", "success"},
             {"/pull?max_cards=250&max_total=1000", snapshot("new", 0)}};
  EXPECT_EQ(client.run(CONNECTION), anki::SyncResult::Success);
  EXPECT_EQ(batchSizes, (std::vector<size_t>{32, 8}));
  ASSERT_EQ(uploadedCards.size(), size_t{40});
  for (size_t i = 0; i < uploadedCards.size(); ++i) EXPECT_EQ(uploadedCards[i], 100 + i % 20);
  EXPECT_EQ(store.pendingCount(), 0U);
  EXPECT_EQ(store.cardCount(), 0);
}

TEST_F(SyncTest, LostPushResponseRetainsReviewsAndNeverAutomaticallyRetries) {
  queueReview();
  replies = {{"/health", HEALTH}, {"/push", "", HttpDownloader::HTTP_ERROR}};
  EXPECT_EQ(client.run(CONNECTION), anki::SyncResult::Attention);
  EXPECT_EQ(store.pendingCount(), 1);
  EXPECT_TRUE(store.needsAttention());
  const auto requests = calls.size();
  EXPECT_EQ(client.run(CONNECTION), anki::SyncResult::Attention);
  EXPECT_EQ(calls.size(), requests);
  anki::AnkiStore restarted;
  ASSERT_TRUE(restarted.open());
  EXPECT_TRUE(restarted.needsAttention());
  EXPECT_EQ(restarted.pendingCount(), 1);
}

TEST_F(SyncTest, PartialDuplicateAndMalformedReceiptsPauseWithoutRetirement) {
  for (const char* receipt : {"partial", "duplicate", "{}"}) {
    Storage = {};
    ASSERT_TRUE(store.open());
    queueReview();
    replies = {{"/health", HEALTH}, {"/push", receipt}};
    EXPECT_EQ(client.run(CONNECTION), anki::SyncResult::Attention) << receipt;
    EXPECT_EQ(store.pendingCount(), 1);
    EXPECT_TRUE(store.needsAttention());
    EXPECT_TRUE(replies.empty());
  }
}

TEST_F(SyncTest, ReceiptPersistenceFailurePreventsFurtherNetworkRequests) {
  queueReview();
  replies = {{"/health", HEALTH}, {"/push", "success", HttpDownloader::OK, true}};
  EXPECT_EQ(client.run(CONNECTION), anki::SyncResult::StorageError);
  EXPECT_EQ(store.pendingCount(), 1);
  EXPECT_TRUE(replies.empty());
}

TEST_F(SyncTest, CompleteNdjsonDoesNotPublishWhenTransportReportsFailure) {
  install();
  replies = {{"/health", HEALTH},
             {"/pull?max_cards=250&max_total=1000", snapshot("new", 2), HttpDownloader::HTTP_ERROR}};
  EXPECT_EQ(client.run(CONNECTION), anki::SyncResult::ConnectionError);
  ASSERT_TRUE(store.open());
  EXPECT_STREQ(store.generation(), "old");
  EXPECT_EQ(store.cardCount(), 1);
}

TEST_F(SyncTest, MalformedStreamPreservesInstalledSnapshotAndReportsProtocolError) {
  install();
  replies = {{"/health", HEALTH}, {"/pull?max_cards=250&max_total=1000", "{bad-json}\n"}};
  EXPECT_EQ(client.run(CONNECTION), anki::SyncResult::ProtocolError);
  ASSERT_TRUE(store.open());
  EXPECT_STREQ(store.generation(), "old");
  EXPECT_EQ(store.cardCount(), 1);
}

TEST_F(SyncTest, TruncatedPullAfterAcknowledgmentKeepsReviewedCardHidden) {
  queueReview();
  auto incomplete = snapshot("new");
  incomplete.erase(incomplete.find("{\"type\":\"end\""));
  replies = {{"/health", HEALTH}, {"/push", "success"}, {"/pull?max_cards=250&max_total=1000", incomplete}};
  EXPECT_EQ(client.run(CONNECTION), anki::SyncResult::ProtocolError);
  ASSERT_TRUE(store.open());
  EXPECT_EQ(store.pendingCount(), 0);
  EXPECT_STREQ(store.generation(), "old");
  anki::CardInfo next;
  EXPECT_FALSE(store.nextUnreviewed(1, 0, next));
}

TEST_F(SyncTest, ChecksDesktopProtocolBeforeAttemptingUpload) {
  queueReview();
  replies = {{"/health", R"({"status":"ok","protocol_version":4,"collection_ready":true})"}};
  EXPECT_EQ(client.run(CONNECTION), anki::SyncResult::ProtocolError);
  EXPECT_EQ(store.pendingCount(), 1);
  EXPECT_FALSE(store.needsAttention());
  EXPECT_TRUE(replies.empty());
}

TEST_F(SyncTest, CancelledBeforeUploadDoesNotCreateUncertainBatch) {
  queueReview();
  const anki::SyncCallbacks callbacks{nullptr, [](void*) { return true; }, nullptr};
  replies = {{"/health", HEALTH}};
  EXPECT_EQ(client.run(CONNECTION, callbacks), anki::SyncResult::Cancelled);
  EXPECT_FALSE(store.needsAttention());
  EXPECT_EQ(store.pendingCount(), 1);
}
}  // namespace
