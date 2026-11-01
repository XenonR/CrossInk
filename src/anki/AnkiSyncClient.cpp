#include "AppCapabilities.h"
#if CROSSINK_APP_CAP_ANKI
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "AnkiStore.h"
#include "AnkiSyncClient.h"
#include "network/HttpDownloader.h"

namespace anki {
bool AnkiSyncClient::initialize(AnkiStore& store) {
  store_ = nullptr;
  // Buffers are activity-owned and reused across batches; keeping ~22 KiB off
  // the small network-task stack also avoids per-response allocation churn.
  if (!decoder_) decoder_ = makeUniqueNoThrow<PullDecoder>();
  if (!batch_) batch_ = makeUniqueNoThrow<UploadBatch>();
  if (!body_) {
    // This 4 KiB request buffer cannot fit on the worker stack. Keep it in
    // PSRAM: X4 Pro's default allocator prefers internal RAM at <= 4 KiB.
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
    body_ = makePsramByteBufferNoThrow(BODY_BYTES);
#else
    body_ = makeHeapByteBufferNoThrow(BODY_BYTES);
#endif
  }
  if (!response_) response_ = makeUniqueNoThrow<char[]>(RESPONSE_BYTES);
  if (!decoder_ || !batch_ || !body_ || !response_) {
    LOG_ERR("ANKI", "Cannot allocate sync workspace");
    return false;
  }
  pullCallbacks_.context = &store;
  pullCallbacks_.onBegin = [](void* context, const char* generation, uint16_t count) {
    return static_cast<AnkiStore*>(context)->beginSnapshot(generation, count);
  };
  pullCallbacks_.onCard = [](void* context, const CardInput& card) {
    return static_cast<AnkiStore*>(context)->appendCard(card);
  };
  // Publication waits until both HTTP completion and decoder finish succeed.
  pullCallbacks_.onEnd = [](void*) { return true; };
  if (!decoder_->initialize(pullCallbacks_)) return false;
  store_ = &store;
  return true;
}

SyncResult AnkiSyncClient::run(const Connection& connection, const SyncCallbacks callbacks) {
  if (!store_ || !decoder_ || !body_ || !response_ || !batch_) return SyncResult::StorageError;
  if (!connection.configured()) return SyncResult::NotConfigured;
  if (store_->failed()) return SyncResult::StorageError;
  if (store_->needsAttention()) return SyncResult::Attention;
  auto* const body = reinterpret_cast<char*>(body_.get());
  const auto cancelled = [&]() { return callbacks.cancelled && callbacks.cancelled(callbacks.context); };
  const auto phase = [&](const SyncPhase value) {
    if (callbacks.phaseChanged) callbacks.phaseChanged(callbacks.context, value);
  };
  size_t received = 0;
  const auto response = [&](const uint8_t* data, const size_t length) {
    if (length > RESPONSE_BYTES - 1 - received) return false;
    memcpy(response_.get() + received, data, length);
    received += length;
    response_[received] = '\0';
    return true;
  };
  int status = 0;
  phase(SyncPhase::Checking);
  auto result = HttpDownloader::requestToken(connection.url + "/health", "", "GET", {}, response, RESPONSE_BYTES - 1,
                                             status, cancelled);
  if (result != HttpDownloader::OK) return cancelled() ? SyncResult::Cancelled : SyncResult::ConnectionError;
  Health health;
  if (!decoder_->parseHealth(response_.get(), received, health) || !health.collectionReady) {
    LOG_ERR("ANKI", "Desktop health/protocol rejected: %s", decoder_->error());
    return SyncResult::ProtocolError;
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
        !decoder_->parsePushReceipt(response_.get(), received, batch_->batchId, batch_->count, receipt) ||
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
  if (!decoder_->initialize(pullCallbacks_)) return SyncResult::StorageError;
  bool decoderRejected = false;
  result = HttpDownloader::requestToken(
      connection.url + "/pull?max_cards=250&max_total=1000", connection.token.c_str(), "GET", {},
      [&](const uint8_t* data, size_t length) {
        if (decoder_->feed(data, length)) return true;
        decoderRejected = true;
        return false;
      },
      PullDecoder::MAX_PULL_BYTES, status, cancelled);
  if (result != HttpDownloader::OK || !decoder_->finish()) {
    store_->cancelSnapshot();
    LOG_ERR("ANKI", "Card download rejected: %s", decoder_->error());
    if (store_->failed()) return SyncResult::StorageError;
    return cancelled()                                       ? SyncResult::Cancelled
           : decoderRejected || result == HttpDownloader::OK ? SyncResult::ProtocolError
                                                             : SyncResult::ConnectionError;
  }
  if (cancelled()) {
    store_->cancelSnapshot();
    return SyncResult::Cancelled;
  }
  if (!store_->commitSnapshot()) return SyncResult::StorageError;
  return SyncResult::Success;
}
}  // namespace anki
#endif
