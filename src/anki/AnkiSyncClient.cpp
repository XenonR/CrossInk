#include "AppCapabilities.h"
#if CROSSINK_APP_CAP_ANKI
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "AnkiStore.h"
#include "AnkiSyncClient.h"
#include "network/HttpDownloader.h"

namespace anki {
namespace {
constexpr char PULL_PATH[] = "/.crossink-anki/pull.tmp";

HeapByteBuffer makeTransferBuffer(const size_t bytes) {
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR) && defined(FREEINK_FB_PSRAM) && FREEINK_FB_PSRAM
  return makePsramByteBufferNoThrow(bytes);
#else
  return makeHeapByteBufferNoThrow(bytes);
#endif
}

void logMemory(const char* phase) {
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  const auto internal = byteHeapSnapshot(MemoryPool::Internal);
  const auto psram = byteHeapSnapshot(MemoryPool::Psram);
  LOG_INF("ANKI", "Sync %s: internal free=%u largest=%u, PSRAM free=%u largest=%u", phase, unsigned(internal.free),
          unsigned(internal.largest), unsigned(psram.free), unsigned(psram.largest));
#else
  (void)phase;
#endif
}
}  // namespace

bool AnkiSyncClient::allocateControlBuffers() {
  // Reuse these bounded buffers for every upload batch, off the 4 KiB worker stack.
  if (!batch_) batch_ = makeUniqueNoThrow<UploadBatch>();
  // S3 buffers explicitly use PSRAM: exhausted PSRAM must not silently consume
  // internal RAM needed by the network stack. C3 uses its ordinary byte heap.
  if (!body_) body_ = makeTransferBuffer(BODY_BYTES);
  if (!response_) response_ = makeTransferBuffer(RESPONSE_BYTES);
  if (!batch_ || !body_ || !response_) {
    LOG_ERR("ANKI", "Cannot allocate sync control workspace");
    return false;
  }
  return true;
}

bool AnkiSyncClient::initialize(AnkiStore& store, const PullLimits limits) {
  store_ = nullptr;
  limits_ = limits;
  if (!decoder_) decoder_ = makeUniqueNoThrow<PullDecoder>();
  if (!decoder_) {
    LOG_ERR("ANKI", "Cannot allocate sync decoder");
    return false;
  }
  if (!allocateControlBuffers()) return false;
  pullCallbacks_.context = this;
  pullCallbacks_.onBegin = [](void* context, const char* generation, uint16_t count) {
    auto& client = *static_cast<AnkiSyncClient*>(context);
    const bool ok = client.store_->beginSnapshot(generation, count);
    if (!ok) client.importFailed_ = true;
    return ok;
  };
  pullCallbacks_.onCard = [](void* context, const CardInput& card) {
    auto& client = *static_cast<AnkiSyncClient*>(context);
    const bool ok = client.store_->appendCard(card);
    if (!ok) client.importFailed_ = true;
    return ok;
  };
  // Publication waits until both HTTP completion and decoder finish succeed.
  pullCallbacks_.onEnd = [](void*) { return true; };
  if (!decoder_->initialize(pullCallbacks_, limits_)) return false;
  store_ = &store;
  return true;
}

SyncResult AnkiSyncClient::run(const Connection& connection, const SyncCallbacks callbacks) {
  if (!store_ || !decoder_) return SyncResult::StorageError;
  if (!connection.configured()) return SyncResult::NotConfigured;
  if (store_->failed()) return SyncResult::StorageError;
  if (store_->needsAttention()) return SyncResult::Attention;
  if (!allocateControlBuffers() || !decoder_->initialize(pullCallbacks_, limits_)) return SyncResult::StorageError;
  importFailed_ = false;
  logMemory("checking");
  // The scratch file is never a resumable snapshot. A later run truncates any
  // interrupted transfer; only the checked binary snapshot can be activated.
  FsFile spool;
  ScopedCleanup cleanup{[&] {
    if (spool && !spool.close()) LOG_ERR("ANKI", "Cannot close download scratch file");
    store_->cancelSnapshot();
    if (Storage.exists(PULL_PATH) && !Storage.remove(PULL_PATH))
      LOG_ERR("ANKI", "Cannot remove download scratch file; next sync will truncate it");
    decoder_->initialize(pullCallbacks_, limits_);
  }};
  auto* const body = reinterpret_cast<char*>(body_.get());
  auto* const responseData = reinterpret_cast<char*>(response_.get());
  const auto cancelled = [&]() { return callbacks.cancelled && callbacks.cancelled(callbacks.context); };
  const auto phase = [&](const SyncPhase value) {
    if (callbacks.phaseChanged) callbacks.phaseChanged(callbacks.context, value);
  };
  size_t received = 0;
  const auto response = [&](const uint8_t* data, const size_t length) {
    if (length > RESPONSE_BYTES - 1 - received) return false;
    memcpy(responseData + received, data, length);
    received += length;
    responseData[received] = '\0';
    return true;
  };
  int status = 0;
  phase(SyncPhase::Checking);
  auto result = HttpDownloader::requestToken(connection.url + "/health", "", "GET", {}, response, RESPONSE_BYTES - 1,
                                             status, cancelled);
  if (result != HttpDownloader::OK) return cancelled() ? SyncResult::Cancelled : SyncResult::ConnectionError;
  Health health;
  if (!decoder_->parseHealth(responseData, received, health) || !health.collectionReady) {
    LOG_ERR("ANKI", "Desktop health/protocol rejected: %s", decoder_->error());
    return decoder_->allocationFailed() ? SyncResult::StorageError : SyncResult::ProtocolError;
  }

  phase(SyncPhase::Uploading);
  while (store_->pendingCount() != 0) {
    if (cancelled()) return SyncResult::Cancelled;
    if (!store_->prepareUpload(*batch_)) return SyncResult::StorageError;
    const size_t bodyLength = serializePush(*batch_, body, BODY_BYTES);
    if (!bodyLength) {
      LOG_ERR("ANKI", "Cannot encode review upload");
      return SyncResult::ProtocolError;
    }
    // Persist before any network I/O. A restart after this point requires
    // reconciliation, because the unchanged plugin cannot prove exactly-once.
    if (!store_->markUploadAttempted(batch_->batchId)) return SyncResult::StorageError;
    received = 0;
    result = HttpDownloader::requestToken(connection.url + "/push", connection.token.c_str(), "POST",
                                          std::string_view(body, bodyLength), response, RESPONSE_BYTES - 1, status,
                                          cancelled);
    PushReceipt receipt;
    if (result != HttpDownloader::OK ||
        !decoder_->parsePushReceipt(responseData, received, batch_->batchId, batch_->count, receipt) ||
        receipt.status != PushReceipt::Status::Success) {
      LOG_ERR("ANKI", "Review upload uncertain; retaining batch for reconciliation");
      if (!store_->markUploadUncertain(batch_->batchId)) return SyncResult::StorageError;
      return SyncResult::Attention;
    }
    // This durable receipt event also retires the exact prepared batch.
    if (!store_->acknowledgeUpload(batch_->batchId)) return SyncResult::StorageError;
  }

  if (cancelled()) return SyncResult::Cancelled;
  phase(SyncPhase::Downloading);
  // Release control-stage allocations before starting the pull. In particular,
  // neither the pull arena nor import identities overlap the HTTP/TLS client.
  decoder_->initialize(pullCallbacks_, limits_);
  body_.reset();
  response_.reset();
  batch_.reset();
  logMemory("downloading");
  if (!Storage.mkdir("/.crossink-anki")) {
    LOG_ERR("ANKI", "Cannot create download scratch directory");
    return SyncResult::StorageError;
  }
  spool = Storage.open(PULL_PATH, O_WRONLY | O_CREAT | O_TRUNC);
  if (!spool) {
    LOG_ERR("ANKI", "Cannot create download scratch file");
    return SyncResult::StorageError;
  }
  bool writeFailed = false;
  const char* const path = limits_.maxCards == C3_PULL_LIMITS.maxCards ? "/pull?max_cards=250&max_total=250"
                                                                       : "/pull?max_cards=250&max_total=1000";
  result = HttpDownloader::requestToken(
      connection.url + path, connection.token.c_str(), "GET", {},
      [&](const uint8_t* data, const size_t length) {
        if (spool.write(data, length) == length) return true;
        LOG_ERR("ANKI", "Cannot write download scratch file");
        writeFailed = true;
        return false;
      },
      PullDecoder::MAX_PULL_BYTES, status, cancelled);
  const bool synced = result == HttpDownloader::OK && spool.sync();
  const bool closed = spool.close();
  if (writeFailed || !closed || (result == HttpDownloader::OK && !synced)) {
    LOG_ERR("ANKI", "Cannot persist complete download scratch file");
    return SyncResult::StorageError;
  }
  if (result != HttpDownloader::OK) return cancelled() ? SyncResult::Cancelled : SyncResult::ConnectionError;
  if (cancelled()) return SyncResult::Cancelled;
  spool = Storage.open(PULL_PATH, O_RDONLY);
  if (!spool) {
    LOG_ERR("ANKI", "Cannot reopen download scratch file");
    return SyncResult::StorageError;
  }
  logMemory("importing");
  struct ReadContext {
    FsFile& file;
    SyncCallbacks callbacks;
    bool failed = false;
    size_t remaining;
  } source{spool, callbacks, false, spool.size()};
  PullSource input;
  input.context = &source;
  input.read = [](void* context, uint8_t* output, const size_t capacity) {
    auto& source = *static_cast<ReadContext*>(context);
    if (!source.remaining) return 0;
    const size_t expected = std::min(capacity, source.remaining);
    const int count = source.file.read(output, expected);
    if (count != static_cast<int>(expected)) {
      LOG_ERR("ANKI", "Cannot read complete download scratch file");
      source.failed = true;
      return -1;
    }
    source.remaining -= static_cast<size_t>(count);
#ifndef SIMULATOR
    delay(0);
#endif
    return count;
  };
  input.cancelled = [](void* context) {
    const auto& source = *static_cast<ReadContext*>(context);
    return source.callbacks.cancelled && source.callbacks.cancelled(source.callbacks.context);
  };
  const bool decoded = decoder_->decode(input);
  const bool readClosed = spool.close();
  if (source.failed || !readClosed || importFailed_ || decoder_->allocationFailed() || store_->failed()) {
    LOG_ERR("ANKI", "Cannot import downloaded card storage");
    return SyncResult::StorageError;
  }
  if (cancelled()) return SyncResult::Cancelled;
  if (!decoded) {
    LOG_ERR("ANKI", "Card download rejected: %s", decoder_->error());
    return SyncResult::ProtocolError;
  }
  if (!store_->commitSnapshot()) return SyncResult::StorageError;
  logMemory("complete");
  return SyncResult::Success;
}
}  // namespace anki
#endif
