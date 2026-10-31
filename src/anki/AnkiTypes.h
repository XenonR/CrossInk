#pragma once

#include <cstddef>
#include <cstdint>

namespace anki {
inline constexpr uint16_t MAX_CARDS = 1000;
inline constexpr uint16_t MAX_DECKS = 1000;
inline constexpr size_t MAX_SIDE_BYTES = 16 * 1024;
inline constexpr size_t MAX_DECK_NAME_BYTES = 240;
inline constexpr size_t MAX_GENERATION_BYTES = 64;
inline constexpr uint8_t MAX_UPLOAD_REVIEWS = 32;
inline constexpr size_t MAX_BATCH_ID_BYTES = 96;

struct CardInput {
  int64_t cardId = 0;
  int64_t deckId = 0;
  const char* deckName = nullptr;
  const char* front = nullptr;
  const char* back = nullptr;
  size_t frontLength = 0;
  size_t backLength = 0;
};

struct CardInfo {
  int64_t cardId = 0;
  int64_t deckId = 0;
  uint16_t ordinal = 0;
};

struct Deck {
  int64_t deckId = 0;
  char name[MAX_DECK_NAME_BYTES + 1] = {};
  uint16_t cardCount = 0;
  uint16_t remaining = 0;
};

struct Review {
  uint32_t sequence = 0;
  uint16_t ordinal = 0;
  int64_t cardId = 0;
  uint8_t ease = 0;
  uint32_t durationMs = 0;
  uint64_t answeredAtMs = 0;
};

struct UploadBatch {
  char batchId[MAX_BATCH_ID_BYTES + 1] = {};
  uint8_t count = 0;
  Review reviews[MAX_UPLOAD_REVIEWS] = {};
};
}  // namespace anki
