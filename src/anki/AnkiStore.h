#pragma once

#include <HalStorage.h>

#include <array>
#include <memory>

#include "AnkiTypes.h"

namespace anki {
// Concrete SD store. Every public operation closes its readers before returning;
// only an in-progress snapshot import retains its own read/write handle.
class AnkiStore {
 public:
  AnkiStore();
  ~AnkiStore();
  bool open();
  void close();
  bool hasSnapshot() const { return generation_[0] != '\0'; }
  uint16_t cardCount() const { return cardCount_; }
  uint16_t deckCount() const { return deckCount_; }
  uint16_t pendingCount() const { return pendingCount_; }
  bool needsAttention() const { return attention_; }
  bool failed() const { return failed_; }
  const char* generation() const { return generation_; }
  bool readDeck(uint16_t ordinal, Deck& out);
  bool readCard(uint16_t ordinal, CardInfo& out);
  bool readSide(uint16_t ordinal, bool answer, char* out, size_t capacity);
  // Returns false without failed() when the selected deck has no remaining cards.
  bool nextUnreviewed(int64_t deckId, uint16_t start, CardInfo& out);
  bool recordReview(uint16_t ordinal, uint8_t ease, uint32_t durationMs, uint64_t answeredAtMs = 0);

  bool beginSnapshot(const char* generation, uint16_t count);
  bool appendCard(const CardInput& card);
  bool commitSnapshot();
  void cancelSnapshot();

  // At most one upload is outstanding. An attempt becomes uncertain BEFORE I/O.
  bool prepareUpload(UploadBatch& out);
  bool markUploadAttempted(const char* batchId);
  bool acknowledgeUpload(const char* batchId);
  bool markUploadUncertain(const char* batchId);
  bool exportRecovery();
  bool resolveAttention();

 private:
  struct ImportState;
  std::unique_ptr<ImportState> import_;
  char generation_[MAX_GENERATION_BYTES + 1] = {};
  std::array<uint8_t, (MAX_CARDS + 7) / 8> reviewed_{};
  uint16_t cardCount_ = 0;
  uint16_t deckCount_ = 0;
  uint16_t pendingCount_ = 0;
  uint32_t nextSequence_ = 1;
  uint32_t acknowledgedThrough_ = 0;
  uint32_t preparedFirst_ = 0;
  uint8_t preparedCount_ = 0;
  bool attempted_ = false;
  bool attention_ = false;
  bool failed_ = false;
  bool appendEvent(uint8_t type, const Review* review = nullptr);
  bool recoverJournal();
  bool validateSnapshot(const char* path, bool adopt);
  bool readReview(uint32_t sequence, Review& out);
  bool repairJournal(size_t validBytes);
  void journalPath(char* out, size_t capacity) const;
  void batchId(char* out, size_t capacity) const;
};
}  // namespace anki
