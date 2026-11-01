#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "AnkiProtocol.h"

namespace {
using namespace anki;

struct Capture {
  std::string generation;
  uint16_t expected = 0;
  std::vector<int64_t> ids;
  std::string front;
  std::string back;
  bool ended = false;
  bool acceptCards = true;

  PullCallbacks callbacks() {
    return {this,
            [](void* context, const char* generation, uint16_t count) {
              auto& capture = *static_cast<Capture*>(context);
              capture.generation = generation;
              capture.expected = count;
              return true;
            },
            [](void* context, const CardInput& card) {
              auto& capture = *static_cast<Capture*>(context);
              capture.ids.push_back(card.cardId);
              capture.front.assign(card.front, card.frontLength);
              capture.back.assign(card.back, card.backLength);
              return capture.acceptCards;
            },
            [](void* context) {
              static_cast<Capture*>(context)->ended = true;
              return true;
            }};
  }
};

std::string metadata(unsigned count, unsigned version = 2) {
  return "{\"type\":\"meta\",\"status\":\"success\",\"protocol_version\":" + std::to_string(version) +
         ",\"pull_id\":\"generation-123\",\"server_time\":1790000000,\"card_count\":" + std::to_string(count) + "}\n";
}

std::string card(const std::string& id = "9223372036854775807", const std::string& front = "Question",
                 const std::string& back = "Answer") {
  JsonDocument document;
  document["type"] = "card";
  document["id"] = id;
  document["deck_id"] = "1700000000000";
  document["deck_name"] = "Languages / Greek";
  document["front"] = front;
  document["back"] = back;
  std::string output;
  serializeJson(document, output);
  return output + '\n';
}

std::string end(unsigned count) { return "{\"type\":\"end\",\"card_count\":" + std::to_string(count) + "}\n"; }

bool feed(PullDecoder& decoder, const std::string& input, const size_t chunk = 257) {
  for (size_t offset = 0; offset < input.size(); offset += chunk) {
    if (!decoder.feed(reinterpret_cast<const uint8_t*>(input.data() + offset), std::min(chunk, input.size() - offset)))
      return false;
  }
  return true;
}

TEST(AnkiProtocol, StreamsAcrossEveryByteAndDefersCommitUntilFinish) {
  Capture capture;
  PullDecoder decoder;
  ASSERT_TRUE(decoder.initialize(capture.callbacks()));
  ASSERT_TRUE(feed(decoder, metadata(1) + card() + end(1), 1)) << decoder.error();
  EXPECT_FALSE(capture.ended);
  ASSERT_TRUE(decoder.finish()) << decoder.error();
  EXPECT_TRUE(capture.ended);
  EXPECT_EQ(capture.ids, std::vector<int64_t>{INT64_MAX});
  EXPECT_EQ(capture.front, "Question");
  EXPECT_EQ(capture.back, "Answer");
  EXPECT_EQ(decoder.protocolVersion(), 2);
  EXPECT_STREQ(decoder.generation(), "generation-123");
}

TEST(AnkiProtocol, SupportsProtocolThreeAndEmptySnapshotsWithoutFinalNewline) {
  Capture capture;
  PullDecoder decoder;
  ASSERT_TRUE(decoder.initialize(capture.callbacks()));
  std::string input = metadata(0, 3) + end(0);
  input.pop_back();
  ASSERT_TRUE(feed(decoder, input));
  ASSERT_TRUE(decoder.finish());
  EXPECT_EQ(decoder.protocolVersion(), 3);
  EXPECT_TRUE(capture.ended);
}

TEST(AnkiProtocol, RejectsTruncationBadCountsAndTrailingRecords) {
  for (const auto& input :
       {metadata(1) + card(), metadata(1) + card() + end(0), metadata(0) + end(0) + card(),
        metadata(1) + card() + end(1) + "junk", metadata(0, 4) + end(0), std::string("{\"type\":\"meta\"}junk\n")}) {
    Capture capture;
    PullDecoder decoder;
    ASSERT_TRUE(decoder.initialize(capture.callbacks()));
    EXPECT_FALSE(feed(decoder, input) && decoder.finish()) << input;
    EXPECT_FALSE(capture.ended);
  }
}

TEST(AnkiProtocol, RejectsEmbeddedNullSuffixesInRecordTypesAndStatuses) {
  for (const auto& input :
       {std::string(
            R"({"type":"meta\u0000junk","status":"success","protocol_version":3,"pull_id":"g","card_count":0})") +
            '\n' + end(0),
        std::string(
            R"({"type":"meta","status":"success\u0000junk","protocol_version":3,"pull_id":"g","card_count":0})") +
            '\n' + end(0),
        metadata(1) + R"({"type":"card\u0000junk","id":1,"deck_id":1,"deck_name":"D","front":"Q","back":"A"})" + '\n' +
            end(1),
        metadata(0) + R"({"type":"end\u0000junk","card_count":0})" + '\n'}) {
    Capture capture;
    PullDecoder decoder;
    ASSERT_TRUE(decoder.initialize(capture.callbacks()));
    EXPECT_FALSE(feed(decoder, input) && decoder.finish());
    EXPECT_FALSE(capture.ended);
  }
}

TEST(AnkiProtocol, RejectsOversizedAndInvalidUtf8SidesWithoutCommitting) {
  for (const auto& front : {std::string(MAX_SIDE_BYTES + 1, 'x'), std::string("before\0after", 12),
                            std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3)}) {
    Capture capture;
    PullDecoder decoder;
    ASSERT_TRUE(decoder.initialize(capture.callbacks()));
    EXPECT_FALSE(feed(decoder, metadata(1) + card("1", front) + end(1)) && decoder.finish());
    EXPECT_FALSE(capture.ended);
  }
}

TEST(AnkiProtocol, AcceptsMaximumUnicodeSidesAndReusesArenaAcrossCards) {
  std::string front, back;
  for (unsigned i = 0; i < 4096; ++i) {
    front += "\xf0\x90\x80\x80";
    back += "\xf0\x90\x80\x81";
  }
  Capture capture;
  PullDecoder decoder;
  ASSERT_TRUE(decoder.initialize(capture.callbacks()));
  ASSERT_TRUE(feed(decoder, metadata(10, 3)));
  for (unsigned i = 1; i <= 10; ++i)
    ASSERT_TRUE(feed(decoder, card(std::to_string(i), front, back))) << decoder.error();
  ASSERT_TRUE(feed(decoder, end(10)));
  ASSERT_TRUE(decoder.finish()) << decoder.error();
  EXPECT_EQ(capture.front, front);
  EXPECT_EQ(capture.back, back);
}

TEST(AnkiProtocol, StreamsLargeUnicodeDeckMetadataWithoutRetainingItsArray) {
  JsonDocument meta;
  meta["type"] = "meta";
  meta["status"] = "success";
  meta["protocol_version"] = 3;
  meta["pull_id"] = "unicode-meta";
  meta["card_count"] = 1000;
  std::string name;
  for (unsigned i = 0; i < 60; ++i) name += "\xf0\x90\x80\x80";
  for (unsigned i = 0; i < 1000; ++i) {
    auto deck = meta["decks"].add<JsonObject>();
    deck["id"] = std::to_string(i + 1);
    deck["name"] = name;
    deck["card_count"] = 1;
  }
  std::string input;
  serializeJson(meta, input);
  ASSERT_GT(input.size(), size_t{128 * 1024});
  Capture capture;
  PullDecoder decoder;
  ASSERT_TRUE(decoder.initialize(capture.callbacks()));
  ASSERT_TRUE(feed(decoder, input + '\n')) << decoder.error();
  for (unsigned i = 0; i < 1000; ++i) ASSERT_TRUE(feed(decoder, card(std::to_string(i + 1)))) << decoder.error();
  ASSERT_TRUE(feed(decoder, end(1000)));
  ASSERT_TRUE(decoder.finish());
  EXPECT_EQ(capture.ids.size(), size_t{1000});
}

TEST(AnkiProtocol, PropagatesStorageFailureAndEnforcesLineBound) {
  Capture capture;
  capture.acceptCards = false;
  PullDecoder decoder;
  ASSERT_TRUE(decoder.initialize(capture.callbacks()));
  EXPECT_FALSE(feed(decoder, metadata(1) + card()));
  EXPECT_FALSE(capture.ended);
  ASSERT_TRUE(decoder.initialize(capture.callbacks()));
  EXPECT_FALSE(feed(decoder, std::string(PullDecoder::MAX_LINE_BYTES + 1, 'x')));
}

TEST(AnkiProtocol, ParsesHealthWithoutAssumingCollectionReady) {
  PullDecoder decoder;
  ASSERT_TRUE(decoder.initialize());
  Health health;
  const std::string input = R"({"status":"ok","protocol_version":3,"collection_ready":false,"server_time":1790000000})";
  ASSERT_TRUE(decoder.parseHealth(input.data(), input.size(), health));
  EXPECT_EQ(health.protocolVersion, 3);
  EXPECT_FALSE(health.collectionReady);
  EXPECT_EQ(health.serverTime, uint64_t{1790000000});
}

TEST(AnkiProtocol, RejectsEmbeddedNullSuffixInHealthStatus) {
  PullDecoder decoder;
  ASSERT_TRUE(decoder.initialize());
  Health health;
  const std::string input =
      R"({"status":"ok\u0000junk","protocol_version":3,"collection_ready":true,"server_time":1790000000})";
  EXPECT_FALSE(decoder.parseHealth(input.data(), input.size(), health));
}

TEST(AnkiProtocol, DistinguishesAcknowledgedPartialAndAmbiguousDuplicateReceipts) {
  PullDecoder decoder;
  ASSERT_TRUE(decoder.initialize());
  PushReceipt receipt;
  for (const auto& status : {std::string("success"), std::string("partial"), std::string("duplicate")}) {
    const std::string rejected = status == "partial" ? R"([{"index":1,"card_id":"2","error":"missing"}])" : "[]";
    const std::string input = "{\"protocol_version\":2,\"batch_id\":\"batch-1\",\"status\":\"" + status +
                              "\",\"processed\":" +
                              (status == "success"   ? "2"
                               : status == "partial" ? "1"
                                                     : "0") +
                              ",\"rejected\":" + rejected + "}";
    ASSERT_TRUE(decoder.parsePushReceipt(input.data(), input.size(), "batch-1", 2, receipt)) << decoder.error();
    if (status == "success") {
      EXPECT_EQ(receipt.status, PushReceipt::Status::Success);
    }
    if (status == "partial") {
      EXPECT_EQ(receipt.status, PushReceipt::Status::Partial);
    }
    if (status == "duplicate") {
      EXPECT_EQ(receipt.status, PushReceipt::Status::Duplicate);
    }
  }
}

TEST(AnkiProtocol, RejectsReceiptsForOtherBatchesMissingReviewsOrDuplicateRejections) {
  for (
      const auto& input :
      {R"({"protocol_version":3,"batch_id":"other","status":"success","processed":2,"rejected":[]})",
       R"({"protocol_version":3,"batch_id":"batch-1","status":"success","processed":1,"rejected":[]})",
       R"({"protocol_version":3,"batch_id":"batch-1","status":"partial","processed":0,"rejected":[{"index":0},{"index":0}]})",
       R"({"protocol_version":3,"batch_id":"batch-1","status":"partial","processed":1,"rejected":[{"index":2}]})"}) {
    PullDecoder decoder;
    ASSERT_TRUE(decoder.initialize());
    PushReceipt receipt;
    EXPECT_FALSE(decoder.parsePushReceipt(input, strlen(input), "batch-1", 2, receipt));
  }
}

TEST(AnkiProtocol, RejectsEmbeddedNullSuffixesInReceiptStatusesAndItemTypes) {
  for (
      const char* input :
      {R"({"protocol_version":3,"batch_id":"batch-1","status":"success\u0000junk","processed":2,"rejected":[]})",
       R"({"protocol_version":3,"batch_id":"batch-1","status":"partial\u0000junk","processed":1,"rejected":[{"index":0}]})",
       R"({"protocol_version":3,"batch_id":"batch-1","status":"duplicate\u0000junk","processed":0,"rejected":[]})",
       R"({"protocol_version":3,"batch_id":"batch-1","status":"partial","processed":1,"rejected":[{"index":0,"kind":"review\u0000junk"}]})"}) {
    PullDecoder decoder;
    ASSERT_TRUE(decoder.initialize());
    PushReceipt receipt;
    EXPECT_FALSE(decoder.parsePushReceipt(input, strlen(input), "batch-1", 2, receipt));
  }
}

TEST(AnkiProtocol, SerializesOrderedReviewsWithOptionalTimestampAndEscapedBatchId) {
  UploadBatch batch;
  strcpy(batch.batchId, "batch-\"\\1");
  batch.count = 2;
  batch.reviews[0] = {1, 0, INT64_MAX, 1, 50, 1790000000000};
  batch.reviews[1] = {2, 1, 1700000000000, 4, 75, 0};
  std::string output(8192, '\0');
  const size_t length = serializePush(batch, output.data(), output.size());
  ASSERT_GT(length, size_t{0});
  JsonDocument document;
  ASSERT_FALSE(deserializeJson(document, output.data(), length));
  EXPECT_STREQ(document["batch_id"], batch.batchId);
  EXPECT_EQ(document["reviews"].size(), size_t{2});
  EXPECT_EQ(document["reviews"][0]["card_id"].as<int64_t>(), INT64_MAX);
  EXPECT_EQ(document["reviews"][0]["answered_at_ms"].as<uint64_t>(), uint64_t{1790000000000});
  EXPECT_TRUE(document["reviews"][1]["answered_at_ms"].isNull());
  EXPECT_EQ(serializePush(batch, output.data(), 20), size_t{0});
  batch.reviews[0].ease = 0;
  EXPECT_EQ(serializePush(batch, output.data(), output.size()), size_t{0});
}

TEST(AnkiProtocol, MaximumUploadFitsFourKiBIncludingWorstCaseEscapedBatchId) {
  UploadBatch batch;
  memset(batch.batchId, '\\', MAX_BATCH_ID_BYTES);
  batch.count = MAX_UPLOAD_REVIEWS;
  for (auto& review : batch.reviews) review = {UINT32_MAX, UINT16_MAX, INT64_MAX, 4, 3600000, 9999999999999ULL};
  std::string output(4096, '\0');
  const size_t length = serializePush(batch, output.data(), output.size());
  ASSERT_GT(length, size_t{0});
  JsonDocument document;
  ASSERT_FALSE(deserializeJson(document, output.data(), length));
  EXPECT_EQ(document["reviews"].size(), size_t{MAX_UPLOAD_REVIEWS});
  EXPECT_EQ(document["batch_id"].as<std::string>(), batch.batchId);
}
}  // namespace
