#pragma once

#include "AppCapabilities.h"
#if CROSSINK_APP_CAP_ANKI
#include <Memory.h>

#include <memory>

#include "AnkiConnectionStore.h"
#include "AnkiProtocol.h"

namespace anki {
class AnkiStore;

enum class SyncPhase : uint8_t { Checking, Uploading, Downloading };
enum class SyncResult : uint8_t {
  Success,
  Cancelled,
  NotConfigured,
  StorageError,
  ConnectionError,
  ProtocolError,
  Attention
};

struct SyncCallbacks {
  void* context = nullptr;
  bool (*cancelled)(void*) = nullptr;
  void (*phaseChanged)(void*, SyncPhase) = nullptr;
};

// Synchronous, foreground-owned client. A worker may own it while the activity
// displays atomic progress; neither the client nor its callbacks navigate/draw.
class AnkiSyncClient {
 public:
  bool initialize(AnkiStore& store);
  SyncResult run(const Connection& connection, SyncCallbacks callbacks = {});

 private:
  static constexpr size_t BODY_BYTES = 4096;
  static constexpr size_t RESPONSE_BYTES = 16384;
  AnkiStore* store_ = nullptr;
  std::unique_ptr<PullDecoder> decoder_;
  std::unique_ptr<UploadBatch> batch_;
  HeapByteBuffer body_;
  std::unique_ptr<char[]> response_;
  PullCallbacks pullCallbacks_;
};
}  // namespace anki
#endif
