#include <gtest/gtest.h>

#include <algorithm>
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
                 const std::string& back = "Answer", const std::string& deckName = "Languages / Greek") {
  JsonDocument document;
  document["type"] = "card";
  document["id"] = id;
  document["deck_id"] = "1700000000000";
  document["deck_name"] = deckName;
  document["front"] = front;
  document["back"] = back;
  std::string output;
  serializeJson(document, output);
  return output + '\n';
}

std::string end(unsigned count) { return "{\"type\":\"end\",\"card_count\":" + std::to_string(count) + "}\n"; }

struct StringSource {
  const std::string& text;
  size_t chunk = 257;
  size_t position = 0;
  size_t failAt = SIZE_MAX;
  size_t cancelAt = SIZE_MAX;

  PullSource source() {
    return {this,
            [](void* context, uint8_t* output, size_t capacity) {
              auto& input = *static_cast<StringSource*>(context);
              if (input.position >= input.failAt) return -1;
              const size_t count =
                  std::min({capacity, input.chunk, input.text.size() - input.position, input.failAt - input.position});
              memcpy(output, input.text.data() + input.position, count);
              input.position += count;
              return static_cast<int>(count);
            },
            [](void* context) {
              const auto& input = *static_cast<StringSource*>(context);
              return input.position >= input.cancelAt;
            }};
  }
};

bool decode(PullDecoder& decoder, const std::string& input, const size_t chunk = 257) {
  StringSource source{input, chunk};
  return decoder.decode(source.source());
}

TEST(AnkiProtocol, StreamsAcrossEveryByteAndFinishesOnlyAfterCompleteInput) {
  Capture capture;
  PullDecoder decoder;
  ASSERT_TRUE(decoder.initialize(capture.callbacks()));
  ASSERT_TRUE(decode(decoder, metadata(1) + card() + end(1), 1)) << decoder.error();
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
  ASSERT_TRUE(decode(decoder, input));
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
    EXPECT_FALSE(decode(decoder, input)) << input;
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
    EXPECT_FALSE(decode(decoder, input));
    EXPECT_FALSE(capture.ended);
  }
}

TEST(AnkiProtocol, RejectsOversizedAndInvalidUtf8SidesWithoutCommitting) {
  for (const auto& front : {std::string(MAX_SIDE_BYTES + 1, 'x'), std::string("before\0after", 12),
                            std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3)}) {
    Capture capture;
    PullDecoder decoder;
    ASSERT_TRUE(decoder.initialize(capture.callbacks()));
    EXPECT_FALSE(decode(decoder, metadata(1) + card("1", front) + end(1)));
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
  std::string input = metadata(10, 3);
  for (unsigned i = 1; i <= 10; ++i) input += card(std::to_string(i), front, back);
  input += end(10);
  ASSERT_TRUE(decode(decoder, input)) << decoder.error();
  EXPECT_EQ(capture.front, front);
  EXPECT_EQ(capture.back, back);
}

TEST(AnkiProtocol, StreamsLargeUnicodeDeckMetadataWithoutRetainingItsArray) {
  std::string input = metadata(1000, 3);
  input.resize(input.size() - 2);  // Replace the final object brace/newline.
  input += ",\"decks\":[";
  std::string name;
  // Desktop's escaped CJK deck names need six wire bytes per UTF-8 character.
  for (unsigned i = 0; i < 80; ++i) name += "\\u4e00";
  for (unsigned i = 0; i < 1000; ++i) {
    if (i) input += ',';
    input += "{\"id\":\"" + std::to_string(i + 1) + "\",\"name\":\"" + name + "\",\"card_count\":1}";
  }
  input += "]}";
  ASSERT_GT(input.size(), size_t{400 * 1024});
  ASSERT_LE(input.size(), PullDecoder::MAX_LINE_BYTES);
  Capture capture;
  PullDecoder decoder;
  ASSERT_TRUE(decoder.initialize(capture.callbacks()));
  input += '\n';
  for (unsigned i = 0; i < 1000; ++i) input += card(std::to_string(i + 1));
  input += end(1000);
  ASSERT_TRUE(decode(decoder, input)) << decoder.error();
  EXPECT_EQ(capture.ids.size(), size_t{1000});
}

TEST(AnkiProtocol, C3AcceptsItsMaximumCardCountAndUnicodeSidesInOneReusedArena) {
  Capture capture;
  PullDecoder decoder;
  anki_protocol_test_memory::reset();
  ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
  EXPECT_EQ(anki_protocol_test_memory::arenaAllocationCalls, size_t{0});
  std::string front, back;
  for (unsigned i = 0; i < C3_PULL_LIMITS.maxSideBytes / 4; ++i) {
    front += "\xf0\x90\x80\x80";
    back += "\xf0\x90\x80\x81";
  }
  const std::string deckName(MAX_DECK_NAME_BYTES, 'D');
  std::string input = metadata(C3_PULL_LIMITS.maxCards, 3);
  for (unsigned i = 1; i <= C3_PULL_LIMITS.maxCards; ++i) input += card(std::to_string(i), front, back, deckName);
  input += end(C3_PULL_LIMITS.maxCards);
  ASSERT_TRUE(decode(decoder, input)) << decoder.error();
  EXPECT_EQ(capture.ids.size(), C3_PULL_LIMITS.maxCards);
  EXPECT_EQ(capture.front, front);
  EXPECT_EQ(capture.back, back);
  EXPECT_EQ(anki_protocol_test_memory::arenaAllocationCalls, size_t{1});
  EXPECT_EQ(anki_protocol_test_memory::lastArenaBytes, C3_PULL_LIMITS.arenaBytes);
}

TEST(AnkiProtocol, C3RejectsLargerNewPullsWithoutChangingStoredCardLimits) {
  EXPECT_EQ(MAX_CARDS, 1000);
  EXPECT_EQ(MAX_SIDE_BYTES, size_t{16 * 1024});
  for (const auto& input :
       {metadata(C3_PULL_LIMITS.maxCards + 1, 3) + end(0),
        metadata(1, 3) + card("1", std::string(C3_PULL_LIMITS.maxSideBytes + 1, 'Q')) + end(1),
        metadata(1, 3) + card("1", "Q", std::string(C3_PULL_LIMITS.maxSideBytes + 1, 'A')) + end(1)}) {
    Capture capture;
    PullDecoder decoder;
    ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
    EXPECT_FALSE(decode(decoder, input));
    EXPECT_FALSE(capture.ended);
    EXPECT_FALSE(decoder.allocationFailed());
  }
}

TEST(AnkiProtocol, AcceptsBlankLinesCrLfAndWhitespaceAfterTheEndRecord) {
  Capture capture;
  PullDecoder decoder;
  ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
  std::string input = "\n \t\r\n  " + metadata(1) + "\r\n\t" + card() + "\n" + end(1) + " \t\r\n";
  ASSERT_TRUE(decode(decoder, input, 3)) << decoder.error();
  EXPECT_TRUE(capture.ended);
}

TEST(AnkiProtocol, EnforcesExactRecordByteLimitWhileSkippingFilteredValues) {
  std::string record = metadata(0);
  record.resize(record.size() - 2);
  record += ",\"ignored\":\"";
  record.resize(PullDecoder::MAX_LINE_BYTES - 2, 'x');
  record += "\"}";
  ASSERT_EQ(record.size(), PullDecoder::MAX_LINE_BYTES);
  for (const bool oversize : {false, true}) {
    Capture capture;
    PullDecoder decoder;
    ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
    std::string input = record;
    if (oversize) input.insert(input.size() - 2, 1, 'x');
    input += '\n' + end(0);
    EXPECT_EQ(decode(decoder, input), !oversize) << decoder.error();
    EXPECT_EQ(capture.ended, !oversize);
  }
}

TEST(AnkiProtocol, RejectsRawNullsInFilteredValuesAfterObjectsAndAfterEnd) {
  std::string filtered = metadata(0);
  filtered.resize(filtered.size() - 2);
  filtered += ",\"decks\":[{\"name\":\"hidden";
  filtered += '\0';
  filtered += "suffix\"}]}\n" + end(0);
  std::string afterObject = metadata(0);
  afterObject.insert(afterObject.size() - 1, 1, '\0');
  afterObject += end(0);
  std::string afterEnd = metadata(0) + end(0);
  afterEnd += '\0';
  for (const auto& input : {filtered, afterObject, afterEnd}) {
    Capture capture;
    PullDecoder decoder;
    ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
    EXPECT_FALSE(decode(decoder, input, 1));
    EXPECT_FALSE(capture.ended);
  }
}

TEST(AnkiProtocol, RejectsTrailingBytesAndNewlinesInsideFilteredStrings) {
  std::string malformed = metadata(0);
  malformed.resize(malformed.size() - 2);
  malformed += ",\"ignored\":\"split\nstring\"}\n" + end(0);
  std::string trailing = metadata(0);
  trailing.insert(trailing.size() - 1, "junk");
  trailing += end(0);
  for (const auto& input : {malformed, trailing}) {
    Capture capture;
    PullDecoder decoder;
    ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
    EXPECT_FALSE(decode(decoder, input, 512));
    EXPECT_FALSE(capture.ended);
  }
}

TEST(AnkiProtocol, ReadErrorsAndCancellationNeverInvokeTheEndCallback) {
  const std::string input = metadata(1) + card() + end(1);
  for (const size_t stop : {size_t{0}, metadata(1).size() + 5, input.size()}) {
    for (const bool cancelled : {false, true}) {
      Capture capture;
      PullDecoder decoder;
      ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
      StringSource source{input, 1};
      if (cancelled)
        source.cancelAt = stop;
      else
        source.failAt = stop;
      EXPECT_FALSE(decoder.decode(source.source())) << "stop=" << stop;
      EXPECT_FALSE(capture.ended);
      EXPECT_FALSE(decoder.allocationFailed());
    }
  }
}

TEST(AnkiProtocol, ChecksCancellationWhileConsumingBufferedBytes) {
  const std::string input = metadata(1) + card() + end(1);
  Capture capture;
  PullDecoder decoder;
  ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
  StringSource source{input, 512};
  source.cancelAt = 1;
  EXPECT_FALSE(decoder.decode(source.source()));
  EXPECT_EQ(source.position, input.size());  // The first read prefetched every record.
  EXPECT_FALSE(capture.ended);
}

TEST(AnkiProtocol, RejectsInvalidReadersAndCanResetAfterFailureOrCompletion) {
  Capture capture;
  PullDecoder decoder;
  EXPECT_FALSE(decode(decoder, metadata(0) + end(0)));
  ASSERT_TRUE(decoder.initialize(capture.callbacks()));
  EXPECT_FALSE(decoder.decode({}));
  ASSERT_TRUE(decoder.initialize(capture.callbacks()));
  PullSource invalid{nullptr, [](void*, uint8_t*, size_t capacity) { return static_cast<int>(capacity + 1); }};
  EXPECT_FALSE(decoder.decode(invalid));
  ASSERT_TRUE(decoder.initialize(capture.callbacks()));
  ASSERT_TRUE(decode(decoder, metadata(0) + end(0)));
  EXPECT_FALSE(decode(decoder, metadata(0) + end(0)));
  ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
  ASSERT_TRUE(decode(decoder, metadata(0, 3) + end(0)));
}

TEST(AnkiProtocol, BoundsTotalWireBytesIncludingWhitespaceAfterTheEndRecord) {
  struct RepeatedSource {
    std::string prefix = metadata(0) + end(0);
    size_t position = 0;
    size_t remaining;
    PullSource source() {
      return {this, [](void* context, uint8_t* output, size_t capacity) {
                auto& source = *static_cast<RepeatedSource*>(context);
                const size_t count = std::min(capacity, source.remaining);
                for (size_t i = 0; i < count; ++i) {
                  output[i] = source.position < source.prefix.size() ? source.prefix[source.position] : ' ';
                  ++source.position;
                }
                source.remaining -= count;
                return static_cast<int>(count);
              }};
    }
  };
  for (const bool oversize : {false, true}) {
    Capture capture;
    PullDecoder decoder;
    ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
    RepeatedSource source{{metadata(0) + end(0)}, 0, PullDecoder::MAX_PULL_BYTES + (oversize ? 1 : 0)};
    EXPECT_EQ(decoder.decode(source.source()), !oversize) << decoder.error();
    EXPECT_EQ(capture.ended, !oversize);
  }
}

TEST(AnkiProtocol, ReusesSmallControlArenaAndAllocatesPullArenaOnlyAfterReset) {
  PullDecoder decoder;
  Capture capture;
  anki_protocol_test_memory::reset();
  ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
  EXPECT_EQ(anki_protocol_test_memory::arenaAllocationCalls, size_t{0});
  Health health;
  const std::string response =
      R"({"status":"ok","protocol_version":3,"collection_ready":true,"server_time":1790000000})";
  for (unsigned i = 0; i < 3; ++i) ASSERT_TRUE(decoder.parseHealth(response.data(), response.size(), health));
  EXPECT_EQ(anki_protocol_test_memory::arenaAllocationCalls, size_t{1});
  EXPECT_EQ(anki_protocol_test_memory::lastArenaBytes, size_t{8192});
  ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
  EXPECT_EQ(anki_protocol_test_memory::arenaAllocationCalls, size_t{1});
  ASSERT_TRUE(decode(decoder, metadata(0) + end(0)));
  EXPECT_EQ(anki_protocol_test_memory::arenaAllocationCalls, size_t{2});
  EXPECT_EQ(anki_protocol_test_memory::lastArenaBytes, C3_PULL_LIMITS.arenaBytes);
}

TEST(AnkiProtocol, AllocationFailuresAreDistinctFromMalformedResponsesAndResetSafely) {
  PullDecoder decoder;
  Capture capture;
  anki_protocol_test_memory::reset();
  ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
  anki_protocol_test_memory::failNextArena = true;
  EXPECT_FALSE(decode(decoder, metadata(0) + end(0)));
  EXPECT_TRUE(decoder.allocationFailed());
  EXPECT_FALSE(capture.ended);
  ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
  EXPECT_FALSE(decoder.allocationFailed());
  ASSERT_TRUE(decode(decoder, metadata(0) + end(0)));
  ASSERT_TRUE(decoder.initialize());
  anki_protocol_test_memory::failNextArena = true;
  Health health;
  const std::string response =
      R"({"status":"ok","protocol_version":3,"collection_ready":true,"server_time":1790000000})";
  EXPECT_FALSE(decoder.parseHealth(response.data(), response.size(), health));
  EXPECT_TRUE(decoder.allocationFailed());
  ASSERT_TRUE(decoder.initialize());
  EXPECT_FALSE(decoder.parseHealth("{bad}", 5, health));
  EXPECT_FALSE(decoder.allocationFailed());
  ASSERT_TRUE(decoder.initialize(capture.callbacks(), {250, 4096, 1}));
  EXPECT_FALSE(decode(decoder, metadata(0) + end(0)));
  EXPECT_FALSE(decoder.allocationFailed());  // Document overflow, not backing allocation failure.
  ASSERT_TRUE(decoder.initialize(capture.callbacks(), C3_PULL_LIMITS));
  EXPECT_TRUE(decode(decoder, metadata(0) + end(0)));
}

TEST(AnkiProtocol, PropagatesStorageFailureAndEnforcesLineBound) {
  Capture capture;
  capture.acceptCards = false;
  PullDecoder decoder;
  ASSERT_TRUE(decoder.initialize(capture.callbacks()));
  EXPECT_FALSE(decode(decoder, metadata(1) + card()));
  EXPECT_FALSE(capture.ended);
  ASSERT_TRUE(decoder.initialize(capture.callbacks()));
  EXPECT_FALSE(decode(decoder, std::string(PullDecoder::MAX_LINE_BYTES + 1, 'x')));
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
