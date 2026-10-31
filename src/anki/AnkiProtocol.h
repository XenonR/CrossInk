#pragma once

#include <AppCapabilities.h>

#if CROSSINK_APP_CAP_ANKI
#include <ArduinoJson.h>
#include <Memory.h>

#include "AnkiTypes.h"

namespace anki {

struct PullCallbacks {
  void* context = nullptr;
  bool (*onBegin)(void*, const char* generation, uint16_t cardCount) = nullptr;
  // Card strings are borrowed until the next decoder operation.
  bool (*onCard)(void*, const CardInput&) = nullptr;
  bool (*onEnd)(void*) = nullptr;
};

struct Health {
  uint8_t protocolVersion = 0;
  bool collectionReady = false;
  uint64_t serverTime = 0;
};

struct PushReceipt {
  enum class Status : uint8_t { Success, Partial, Duplicate };
  Status status = Status::Partial;
  uint16_t processed = 0;
  uint16_t rejected = 0;
};

// One activity-owned workspace keeps parsing independent of HTTP and SD I/O.
class PullDecoder {
 public:
  static constexpr size_t MAX_LINE_BYTES = 512 * 1024;
  static constexpr size_t ARENA_BYTES = 128 * 1024;
  static constexpr size_t MAX_PULL_BYTES = 64 * 1024 * 1024;

  PullDecoder();
  bool initialize(PullCallbacks callbacks = {});
  bool feed(const uint8_t* data, size_t length);
  bool finish();
  uint8_t protocolVersion() const { return version_; }
  const char* generation() const { return generation_; }
  const char* error() const { return error_; }

  bool parseHealth(const char* data, size_t length, Health& health);
  bool parsePushReceipt(const char* data, size_t length, const char* expectedBatch, uint16_t expectedCount,
                        PushReceipt& receipt);

 private:
  class Arena final : public ArduinoJson::Allocator {
   public:
    bool initialize();
    void reset() { used_ = 0; }
    void* allocate(size_t size) override;
    void deallocate(void*) override {}
    void* reallocate(void* ptr, size_t size) override;

   private:
    HeapByteBuffer storage_;
    size_t used_ = 0;
  };

  enum class State : uint8_t { Uninitialized, Meta, Cards, Ended, Finished, Failed };
  bool parseJson(const char* data, size_t length);
  bool processLine();
  bool fail(const char* message);

  Arena arena_;
  JsonDocument document_;
  HeapByteBuffer line_;
  PullCallbacks callbacks_;
  State state_ = State::Uninitialized;
  size_t lineLength_ = 0;
  size_t receivedBytes_ = 0;
  uint16_t expectedCount_ = 0;
  uint16_t cardCount_ = 0;
  uint8_t version_ = 0;
  char generation_[MAX_GENERATION_BYTES + 1] = {};
  const char* error_ = "";
};

// Returns the JSON byte count, or zero if the batch or output capacity is invalid.
size_t serializePush(const UploadBatch& batch, char* output, size_t capacity);

}  // namespace anki
#endif
