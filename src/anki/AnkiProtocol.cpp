#include "AnkiProtocol.h"

#if CROSSINK_APP_CAP_ANKI
#include <Logging.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

namespace anki {
namespace {
struct alignas(std::max_align_t) AllocationHeader {
  size_t size;
  size_t capacity;
};

bool whitespace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool supportedVersion(const JsonVariantConst value, uint8_t& version) {
  if (!value.is<unsigned>()) return false;
  const unsigned parsed = value.as<unsigned>();
  if (parsed != 2 && parsed != 3) return false;
  version = static_cast<uint8_t>(parsed);
  return true;
}

bool validUtf8(const char* text, const size_t size) {
  size_t i = 0;
  while (i < size) {
    const auto first = static_cast<uint8_t>(text[i++]);
    if (first < 0x80) {
      if (first == 0) return false;
      continue;
    }
    unsigned remaining;
    uint32_t codepoint;
    uint32_t minimum;
    if (first >= 0xc2 && first <= 0xdf) {
      remaining = 1;
      codepoint = first & 0x1f;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      remaining = 2;
      codepoint = first & 0x0f;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      remaining = 3;
      codepoint = first & 0x07;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (remaining > size - i) return false;
    while (remaining--) {
      const auto next = static_cast<uint8_t>(text[i++]);
      if ((next & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
  }
  return true;
}

bool stringValue(const JsonVariantConst value, const size_t maximum, JsonString& text) {
  if (!value.is<const char*>()) return false;
  text = value.as<JsonString>();
  return text.size() <= maximum && validUtf8(text.c_str(), text.size());
}

bool equals(const JsonVariantConst value, const std::string_view expected) {
  if (!value.is<const char*>()) return false;
  const JsonString text = value.as<JsonString>();
  return text.size() == expected.size() && memcmp(text.c_str(), expected.data(), expected.size()) == 0;
}

bool positiveId(const JsonVariantConst value, int64_t& id) {
  if (value.is<int64_t>()) {
    id = value.as<int64_t>();
    return id > 0;
  }
  JsonString text;
  if (!stringValue(value, 19, text) || text.size() == 0) return false;
  uint64_t parsed = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const char digit = text.c_str()[i];
    if (digit < '0' || digit > '9') return false;
    const unsigned number = static_cast<unsigned>(digit - '0');
    if (parsed > (static_cast<uint64_t>(INT64_MAX) - number) / 10) return false;
    parsed = parsed * 10 + number;
  }
  id = static_cast<int64_t>(parsed);
  return id > 0;
}

bool boundedAscii(const char* text, const size_t size, const size_t maximum) {
  if (!size || size > maximum) return false;
  for (size_t i = 0; i < size; ++i) {
    if (text[i] < 33 || text[i] > 126) return false;
  }
  return true;
}

// ArduinoJson owns the grammar; this adapter only records the consumed byte count.
struct Input {
  const char* data;
  size_t size;
  size_t position = 0;
  int read() { return position < size ? static_cast<uint8_t>(data[position++]) : -1; }
  size_t readBytes(char* output, const size_t length) {
    const size_t count = std::min(length, size - position);
    memcpy(output, data + position, count);
    position += count;
    return count;
  }
};
}  // namespace

PullDecoder::PullDecoder() : document_(&arena_) {}

bool PullDecoder::Arena::initialize(const size_t capacity) {
  if (storage_ && capacity_ >= capacity) return true;
  release();
  // The bounded arena is activity-owned, reused for every record, and too
  // large for the worker stack. S3 profiles keep parsing out of internal RAM.
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
#if defined(FREEINK_FB_PSRAM) && FREEINK_FB_PSRAM
  storage_ = makeAlignedByteBufferNoThrow(capacity, MemoryPool::Psram);
#else
  storage_ = makeAlignedByteBufferNoThrow(capacity, MemoryPool::Internal);
#endif
#else
  storage_ = makeAlignedByteBufferNoThrow(capacity);
#endif
  if (storage_) capacity_ = capacity;
  return static_cast<bool>(storage_);
}

void PullDecoder::Arena::release() {
  storage_.reset();
  capacity_ = used_ = 0;
}

void* PullDecoder::Arena::allocate(const size_t size) {
  constexpr size_t alignment = alignof(std::max_align_t);
  if (size > capacity_ || capacity_ < sizeof(AllocationHeader) || used_ > capacity_ - sizeof(AllocationHeader))
    return nullptr;
  const size_t capacity = (size + alignment - 1) & ~(alignment - 1);
  if (capacity > capacity_ - used_ - sizeof(AllocationHeader)) return nullptr;
  auto* header = reinterpret_cast<AllocationHeader*>(storage_.get() + used_);
  header->size = size;
  header->capacity = capacity;
  used_ += sizeof(AllocationHeader) + capacity;
  return header + 1;
}

void* PullDecoder::Arena::reallocate(void* ptr, const size_t size) {
  if (!ptr) return allocate(size);
  auto* header = static_cast<AllocationHeader*>(ptr) - 1;
  if (size <= header->capacity) {
    header->size = size;
    return ptr;  // ArduinoJson requires shrinking reallocations to succeed.
  }
  const size_t offset = reinterpret_cast<uint8_t*>(header) - storage_.get();
  if (offset + sizeof(AllocationHeader) + header->capacity == used_) {
    const size_t previousUsed = used_;
    used_ = offset;
    if (void* resized = allocate(size)) return resized;
    used_ = previousUsed;
    return nullptr;
  }
  void* replacement = allocate(size);
  if (replacement) memcpy(replacement, ptr, std::min(header->size, size));
  return replacement;
}

bool PullDecoder::fail(const char* message) {
  error_ = message;
  state_ = State::Failed;
  LOG_ERR("ANKI", "%s", message);
  return false;
}

bool PullDecoder::initialize(const PullCallbacks callbacks, const PullLimits limits) {
  document_.clear();
  arena_.release();
  callbacks_ = callbacks;
  limits_ = limits;
  source_ = {};
  state_ = State::Meta;
  inputPosition_ = inputLength_ = 0;
  sourceEnded_ = lineEnded_ = false;
  allocationFailed_ = false;
  lineLength_ = receivedBytes_ = 0;
  expectedCount_ = cardCount_ = 0;
  version_ = 0;
  generation_[0] = '\0';
  error_ = "";
  if (!limits.maxCards || limits.maxCards > MAX_CARDS || !limits.maxSideBytes || limits.maxSideBytes > MAX_SIDE_BYTES ||
      !limits.arenaBytes || limits.arenaBytes > FULL_PULL_LIMITS.arenaBytes)
    return fail("Invalid Anki download limits");
  return true;
}

bool PullDecoder::parseJson(const char* data, const size_t length) {
  constexpr size_t CONTROL_ARENA_BYTES = 8 * 1024;
  constexpr size_t MAX_CONTROL_BYTES = 16 * 1024;
  allocationFailed_ = false;
  if (state_ == State::Uninitialized || !data || !length || length > MAX_CONTROL_BYTES || memchr(data, '\0', length))
    return fail("Invalid Anki JSON record size");
  document_.clear();
  arena_.reset();
  if (!arena_.initialize(CONTROL_ARENA_BYTES)) {
    allocationFailed_ = true;
    return fail("Could not allocate Anki control parser arena");
  }
  Input input{data, length};
  // Control responses are already byte-bounded. Skipping the filter avoids a
  // second JSON document and keeps their shared workspace within 8 KiB.
  const auto parsed = deserializeJson(document_, input, DeserializationOption::NestingLimit(8));
  if (parsed || document_.overflowed()) return fail("Invalid or oversized Anki JSON record");
  if (!document_.is<JsonObject>()) return fail("Anki JSON record must be an object");
  while (input.position < length) {
    if (!whitespace(data[input.position++])) return fail("Trailing data in Anki JSON record");
  }
  return true;
}

int PullDecoder::LineInput::read() {
  if (first >= 0) {
    const int value = first;
    first = -1;
    return value;
  }
  return decoder.readLineByte();
}

size_t PullDecoder::LineInput::readBytes(char* output, const size_t length) {
  size_t count = 0;
  while (count < length) {
    const int value = read();
    if (value < 0) break;
    output[count++] = static_cast<char>(value);
  }
  return count;
}

int PullDecoder::readByte() {
  if (state_ == State::Failed || sourceEnded_) return -1;
  if (source_.cancelled && source_.cancelled(source_.context)) {
    fail("Anki download cancelled");
    return -1;
  }
  if (inputPosition_ == inputLength_) {
    const int count = source_.read(source_.context, input_, sizeof(input_));
    if (count < 0 || static_cast<size_t>(count) > sizeof(input_)) {
      fail("Could not read Anki download");
      return -1;
    }
    if (!count) {
      sourceEnded_ = true;
      return -1;
    }
    inputPosition_ = 0;
    inputLength_ = static_cast<size_t>(count);
  }
  if (receivedBytes_ == MAX_PULL_BYTES) {
    fail("Anki download exceeds size limit");
    return -1;
  }
  ++receivedBytes_;
  const int value = input_[inputPosition_++];
  if (!value) {
    fail("Anki download contains a null byte");
    return -1;
  }
  return value;
}

int PullDecoder::readLineByte() {
  if (lineEnded_) return -1;
  const int value = readByte();
  if (value < 0) return -1;
  if (value == '\n') {
    lineEnded_ = true;
    return -1;
  }
  if (lineLength_ == MAX_LINE_BYTES) {
    fail("Anki JSON line exceeds size limit");
    return -1;
  }
  ++lineLength_;
  return value;
}

bool PullDecoder::parseLine(const int first) {
  document_.clear();
  arena_.reset();
  if (!arena_.initialize(limits_.arenaBytes)) {
    allocationFailed_ = true;
    return fail("Could not allocate Anki download parser arena");
  }
  JsonDocument filter(&arena_);
  static constexpr const char* fields[] = {
      "type",  "status", "protocol_version", "pull_id",  "server_time", "card_count", "id", "deck_id", "deck_name",
      "front", "back",   "collection_ready", "batch_id", "processed",   "rejected"};
  for (const char* field : fields) filter[field] = true;
  LineInput input{*this, first};
  const auto parsed =
      deserializeJson(document_, input, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(8));
  if (state_ == State::Failed) return false;
  if (parsed || document_.overflowed()) return fail("Invalid or oversized Anki JSON record");
  if (!document_.is<JsonObject>()) return fail("Anki JSON record must be an object");
  int value;
  while ((value = input.read()) >= 0) {
    if (!whitespace(static_cast<char>(value))) return fail("Trailing data in Anki JSON record");
  }
  return state_ != State::Failed;
}

bool PullDecoder::processRecord() {
  const JsonVariantConst type = document_["type"];
  if (state_ == State::Meta) {
    JsonString generation;
    if (!equals(type, "meta") || !equals(document_["status"], "success") ||
        !supportedVersion(document_["protocol_version"], version_) || !document_["card_count"].is<unsigned>() ||
        document_["card_count"].as<unsigned>() > limits_.maxCards ||
        !stringValue(document_["pull_id"], MAX_GENERATION_BYTES, generation) ||
        !boundedAscii(generation.c_str(), generation.size(), MAX_GENERATION_BYTES))
      return fail("Unsupported or invalid Anki download metadata");
    memcpy(generation_, generation.c_str(), generation.size());
    generation_[generation.size()] = '\0';
    expectedCount_ = document_["card_count"].as<uint16_t>();
    if (!callbacks_.onBegin || !callbacks_.onBegin(callbacks_.context, generation_, expectedCount_))
      return fail("Could not begin Anki snapshot");
    state_ = State::Cards;
    return true;
  }
  if (state_ != State::Cards) return fail("Unexpected record after Anki download end");
  if (equals(type, "end")) {
    if (!document_["card_count"].is<unsigned>() || document_["card_count"].as<unsigned>() != cardCount_ ||
        cardCount_ != expectedCount_)
      return fail("Anki download card counts do not match");
    state_ = State::Ended;
    return true;
  }
  if (!equals(type, "card") || cardCount_ >= expectedCount_) return fail("Unexpected Anki card record");
  CardInput card;
  JsonString deckName, front, back;
  if (!positiveId(document_["id"], card.cardId) || !positiveId(document_["deck_id"], card.deckId) ||
      !stringValue(document_["deck_name"], MAX_DECK_NAME_BYTES, deckName) || !deckName.size() ||
      !stringValue(document_["front"], limits_.maxSideBytes, front) ||
      !stringValue(document_["back"], limits_.maxSideBytes, back))
    return fail("Invalid or oversized Anki card content");
  card.deckName = deckName.c_str();
  card.front = front.c_str();
  card.back = back.c_str();
  card.frontLength = front.size();
  card.backLength = back.size();
  if (!callbacks_.onCard || !callbacks_.onCard(callbacks_.context, card)) return fail("Could not store Anki card");
  ++cardCount_;
  return true;
}

bool PullDecoder::decode(const PullSource source) {
  allocationFailed_ = false;
  if (state_ != State::Meta) return false;
  if (!source.read) return fail("Missing Anki download reader");
  source_ = source;
  while (!sourceEnded_ && state_ != State::Ended) {
    lineLength_ = 0;
    lineEnded_ = false;
    int first;
    do {
      first = readLineByte();
    } while (first >= 0 && whitespace(static_cast<char>(first)));
    if (state_ == State::Failed) return false;
    if (first < 0) continue;  // Blank line or clean EOF.
    if (!parseLine(first) || !processRecord()) return false;
  }
  if (state_ == State::Failed) return false;
  if (state_ != State::Ended) return fail("Anki download ended before its end record");
  int value;
  while ((value = readByte()) >= 0) {
    if (!whitespace(static_cast<char>(value))) return fail("Unexpected data after Anki download end");
  }
  if (state_ == State::Failed) return false;
  if (!callbacks_.onEnd || !callbacks_.onEnd(callbacks_.context)) return fail("Could not finish Anki snapshot");
  state_ = State::Finished;
  return true;
}

bool PullDecoder::parseHealth(const char* data, const size_t length, Health& health) {
  health = {};
  if (!parseJson(data, length)) return false;
  if (!equals(document_["status"], "ok") || !supportedVersion(document_["protocol_version"], health.protocolVersion) ||
      !document_["collection_ready"].is<bool>() || !document_["server_time"].is<uint64_t>())
    return fail("Unsupported or invalid Anki server health response");
  health.collectionReady = document_["collection_ready"].as<bool>();
  health.serverTime = document_["server_time"].as<uint64_t>();
  return true;
}

bool PullDecoder::parsePushReceipt(const char* data, const size_t length, const char* expectedBatch,
                                   const uint16_t expectedCount, PushReceipt& receipt) {
  receipt = {};
  if (!expectedBatch || expectedCount == 0 || expectedCount > MAX_UPLOAD_REVIEWS || !parseJson(data, length))
    return false;
  uint8_t version;
  JsonString batch;
  if (!supportedVersion(document_["protocol_version"], version) ||
      !stringValue(document_["batch_id"], MAX_BATCH_ID_BYTES, batch) || strcmp(batch.c_str(), expectedBatch) ||
      !document_["processed"].is<unsigned>() || document_["processed"].as<unsigned>() > expectedCount ||
      !document_["rejected"].is<JsonArray>())
    return fail("Anki upload acknowledgement does not match its batch");
  receipt.processed = document_["processed"].as<uint16_t>();
  const JsonArrayConst rejected = document_["rejected"];
  if (rejected.size() > expectedCount) return fail("Invalid Anki rejection count");
  receipt.rejected = static_cast<uint16_t>(rejected.size());
  uint32_t seen = 0;
  for (const JsonVariantConst entry : rejected) {
    if (!entry["index"].is<unsigned>() || entry["index"].as<unsigned>() >= expectedCount)
      return fail("Invalid Anki rejected review index");
    const uint32_t bit = uint32_t{1} << entry["index"].as<unsigned>();
    if (seen & bit) return fail("Duplicate Anki rejected review index");
    seen |= bit;
    if (!entry["kind"].isNull() && !equals(entry["kind"], "review")) return fail("Unexpected Anki rejected item type");
  }
  const JsonVariantConst status = document_["status"];
  if (equals(status, "success") && receipt.processed == expectedCount && receipt.rejected == 0) {
    receipt.status = PushReceipt::Status::Success;
    return true;
  }
  if (equals(status, "partial") && receipt.rejected > 0 && receipt.processed + receipt.rejected == expectedCount) {
    receipt.status = PushReceipt::Status::Partial;
    return true;
  }
  if (equals(status, "duplicate") && receipt.processed == 0 && receipt.rejected == 0) {
    receipt.status = PushReceipt::Status::Duplicate;
    return true;
  }
  return fail("Invalid Anki upload acknowledgement status");
}

size_t serializePush(const UploadBatch& batch, char* output, const size_t capacity) {
  if (!output || !capacity || batch.count == 0 || batch.count > MAX_UPLOAD_REVIEWS) return 0;
  const size_t idLength = strnlen(batch.batchId, sizeof(batch.batchId));
  if (!boundedAscii(batch.batchId, idLength, MAX_BATCH_ID_BYTES)) return 0;
  size_t used = 0;
  auto append = [&](const char* text, const size_t length) {
    if (length >= capacity - used) return false;
    memcpy(output + used, text, length);
    used += length;
    output[used] = '\0';
    return true;
  };
  if (!append("{\"batch_id\":\"", 13)) return 0;
  for (size_t i = 0; i < idLength; ++i) {
    const char value = batch.batchId[i];
    if ((value == '\\' || value == '"') && !append("\\", 1)) return 0;
    if (!append(&value, 1)) return 0;
  }
  if (!append("\",\"reviews\":[", 13)) return 0;
  for (uint8_t i = 0; i < batch.count; ++i) {
    const Review& review = batch.reviews[i];
    if (review.cardId <= 0 || review.ease < 1 || review.ease > 4 || review.durationMs > 3600000 ||
        review.answeredAtMs > 9999999999999ULL)
      return 0;
    char item[160];
    const int length = snprintf(item, sizeof(item), "%s{\"card_id\":%" PRId64 ",\"ease\":%u,\"duration_ms\":%" PRIu32,
                                i ? "," : "", review.cardId, static_cast<unsigned>(review.ease), review.durationMs);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(item) || !append(item, static_cast<size_t>(length)))
      return 0;
    if (review.answeredAtMs) {
      const int timestampLength = snprintf(item, sizeof(item), ",\"answered_at_ms\":%" PRIu64, review.answeredAtMs);
      if (timestampLength <= 0 || static_cast<size_t>(timestampLength) >= sizeof(item) ||
          !append(item, static_cast<size_t>(timestampLength)))
        return 0;
    }
    if (!append("}", 1)) return 0;
  }
  return append("]}", 2) ? used : 0;
}
}  // namespace anki
#endif
