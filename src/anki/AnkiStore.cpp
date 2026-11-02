#include <AppCapabilities.h>

#if CROSSINK_APP_CAP_ANKI
#include <Logging.h>
#include <Memory.h>
#include <uzlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include "AnkiStore.h"

namespace anki {
namespace {
constexpr char ROOT[] = "/.crossink-anki";
constexpr char SNAPSHOT[] = "/.crossink-anki/cards.bin";
constexpr char STAGED[] = "/.crossink-anki/cards.tmp";
constexpr char BACKUP[] = "/.crossink-anki/cards.bak";
constexpr size_t HEADER_BYTES = 128;
constexpr size_t CARD_BYTES = 40;
constexpr size_t DECK_BYTES = 256;
constexpr size_t JOURNAL_HEADER_BYTES = 96;
constexpr size_t EVENT_BYTES = 64;
constexpr uint16_t JOURNAL_VERSION = 3;
constexpr uint8_t RATING = 1, PREPARED = 2, ATTEMPTED = 3, ACKNOWLEDGED = 4, UNCERTAIN = 5, RESOLVED = 6,
                  RESTART_DECK = 7, LEARNING_RATING = 8;
constexpr uint32_t OTHER_DECK = std::numeric_limits<uint32_t>::max();
constexpr uint32_t AGAIN_GAP = 2, HARD_GAP = 5;

uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t get64(const uint8_t* p) { return get32(p) | (uint64_t(get32(p + 4)) << 32); }
void put16(uint8_t* p, uint16_t n) {
  p[0] = n;
  p[1] = n >> 8;
}
void put32(uint8_t* p, uint32_t n) {
  for (int i = 0; i < 4; ++i) p[i] = n >> (i * 8);
}
void put64(uint8_t* p, uint64_t n) {
  put32(p, n);
  put32(p + 4, n >> 32);
}
uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0xffffffffU) {
  // Reuse the firmware's nibble-table CRC without changing the on-SD checksum
  // or this store's unfinalized incremental accumulator.
  return ~uzlib_crc32(p, static_cast<unsigned>(n), ~crc);
}
uint64_t generationHash(const char* generation) {
  uint64_t hash = 14695981039346656037ULL;
  for (; *generation; ++generation) {
    hash ^= static_cast<uint8_t>(*generation);
    hash *= 1099511628211ULL;
  }
  return hash;
}
bool validGeneration(const char* generation) {
  if (!generation || !*generation || strlen(generation) > MAX_GENERATION_BYTES) return false;
  for (const char* p = generation; *p; ++p) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
          *p == '.'))
      return false;
  }
  return true;
}
bool finishFile(FsFile& file, bool ok) {
  const bool synced = ok && file.sync();
  const bool closed = file.close();
  if (!synced || !closed) LOG_ERR("ANKI", "SD write/sync/close failed");
  return synced && closed;
}
bool exactRead(FsFile& file, void* p, size_t n) { return file.read(p, n) == static_cast<int>(n); }
bool payloadCrc(FsFile& file, uint32_t& result) {
  uint8_t buffer[256];  // Bounded sector transfer scratch; never a card-sized stack allocation.
  if (!file.seek(HEADER_BYTES)) return false;
  uint32_t crc = 0xffffffffU;
  size_t remaining = file.size() - HEADER_BYTES;
  while (remaining) {
    const size_t n = std::min(remaining, sizeof(buffer));
    if (!exactRead(file, buffer, n)) return false;
    crc = crc32(buffer, n, crc);
    remaining -= n;
  }
  result = crc ^ 0xffffffffU;
  return true;
}
void decodeReview(const uint8_t* bytes, Review& review) {
  review.sequence = get32(bytes + 8);
  review.ordinal = get16(bytes + 12);
  review.ease = bytes[14];
  review.durationMs = get32(bytes + 16);
  review.cardId = static_cast<int64_t>(get64(bytes + 24));
  review.answeredAtMs = get64(bytes + 32);
}
}  // namespace

struct AnkiStore::ImportState {
  FsFile file;
  char generation[MAX_GENERATION_BYTES + 1] = {};
  uint16_t expected = 0, count = 0, decks = 0;
  uint32_t end = 0;
  // One import-only array: count card IDs followed by at most count deck IDs.
  // Allocate once off the task stack: 4,000 bytes at 250 cards, 16,000 at 1,000.
  std::unique_ptr<int64_t[]> identities;
};

AnkiStore::AnkiStore() = default;
AnkiStore::~AnkiStore() { close(); }
void AnkiStore::close() {
  cancelSnapshot();
  reviewSchedule_.reset();
  reviewDeckId_ = 0;
  reviewTurn_ = 0;
  lastWasRetry_ = false;
}

void AnkiStore::journalPath(char* out, size_t capacity) const {
  snprintf(out, capacity, "%s/reviews-%016llx.log", ROOT, static_cast<unsigned long long>(generationHash(generation_)));
}
void AnkiStore::batchId(char* out, size_t capacity) const {
  snprintf(out, capacity, "ci-%016llx-%08lx-%02x", static_cast<unsigned long long>(generationHash(generation_)),
           static_cast<unsigned long>(preparedFirst_), preparedCount_);
}

bool AnkiStore::validateSnapshot(const char* path, bool adopt) {
  auto file = Storage.open(path, O_RDONLY);
  uint8_t header[HEADER_BYTES] = {};
  bool ok = file && exactRead(file, header, sizeof(header));
  if (ok) {
    ok = memcmp(header, "ANK1", 4) == 0 && get16(header + 4) == 1 && get16(header + 6) <= MAX_CARDS &&
         get16(header + 8) <= get16(header + 6) && get16(header + 10) == HEADER_BYTES &&
         get32(header + 12) == file.size() && header[28] > 0 && header[28] <= MAX_GENERATION_BYTES &&
         header[32 + header[28]] == 0 && validGeneration(reinterpret_cast<const char*>(header + 32)) &&
         get32(header + 124) == (crc32(header, 124) ^ 0xffffffffU);
  }
  if (ok) {
    uint32_t crc = 0;
    ok = file.size() >= HEADER_BYTES + get16(header + 6) * (CARD_BYTES + DECK_BYTES) && payloadCrc(file, crc) &&
         crc == get32(header + 16);
  }
  if (file) ok = file.close() && ok;
  if (ok && adopt) {
    memcpy(generation_, header + 32, MAX_GENERATION_BYTES + 1);
    cardCount_ = get16(header + 6);
    deckCount_ = get16(header + 8);
  }
  if (!ok) LOG_ERR("ANKI", "Invalid or unreadable snapshot %s", path);
  return ok;
}

bool AnkiStore::open() {
  close();
  generation_[0] = 0;
  cardCount_ = deckCount_ = pendingCount_ = 0;
  journalVersion_ = 0;
  preparedFirst_ = preparedCount_ = 0;
  nextSequence_ = 1;
  acknowledgedThrough_ = 0;
  reviewReadOffset_ = JOURNAL_HEADER_BYTES;
  reviewReadSequence_ = 1;
  attempted_ = attention_ = failed_ = false;
  reviewed_.fill(0);
  if (!Storage.exists(SNAPSHOT)) {
    if (!Storage.exists(BACKUP)) return true;
    // Missing current is the interrupted old->backup/new->current rename window.
    if (!validateSnapshot(BACKUP, false) || !Storage.rename(BACKUP, SNAPSHOT)) {
      LOG_ERR("ANKI", "Snapshot recovery failed");
      failed_ = true;
      return false;
    }
  }
  // A corrupt installed current may own pending reviews; never silently roll it
  // back to an older generation. Keep all files for explicit recovery instead.
  if (!validateSnapshot(SNAPSHOT, true) || !recoverJournal()) {
    LOG_ERR("ANKI", "Anki state needs storage recovery");
    failed_ = true;
    return false;
  }
  return true;
}

bool AnkiStore::beginSnapshot(const char* generation, uint16_t count) {
  cancelSnapshot();
  if (failed_ || pendingCount_ || attention_ || !validGeneration(generation) || count > MAX_CARDS ||
      (hasSnapshot() && strcmp(generation_, generation) == 0)) {
    LOG_ERR("ANKI", "Cannot replace pending/invalid snapshot");
    return false;
  }
  if (!Storage.mkdir(ROOT)) {
    LOG_ERR("ANKI", "Cannot create Anki directory");
    return false;
  }
  import_ = makeUniqueNoThrow<ImportState>();
  if (!import_) {
    LOG_ERR("ANKI", "Cannot allocate import metadata");
    return false;
  }
  if (count) {
    import_->identities = makeUniqueNoThrow<int64_t[]>(size_t(count) * 2);
    if (!import_->identities) {
      LOG_ERR("ANKI", "Cannot allocate %u-byte import identities", unsigned(size_t(count) * 2 * sizeof(int64_t)));
      import_.reset();
      return false;
    }
  }
  memcpy(import_->generation, generation, strlen(generation) + 1);
  import_->expected = count;
  import_->end = HEADER_BYTES + count * (CARD_BYTES + DECK_BYTES);
  import_->file = Storage.open(STAGED, O_RDWR | O_CREAT | O_TRUNC);
  uint8_t zeros[256] = {};
  bool ok = static_cast<bool>(import_->file);
  for (size_t written = 0; ok && written < import_->end;) {
    const size_t n = std::min<size_t>(sizeof(zeros), import_->end - written);
    ok = import_->file.write(zeros, n) == n;
    written += n;
  }
  if (!ok) {
    LOG_ERR("ANKI", "Cannot stage Anki snapshot");
    cancelSnapshot();
  }
  return ok;
}

bool AnkiStore::appendCard(const CardInput& card) {
  if (!import_ || import_->count >= import_->expected || card.cardId <= 0 || card.deckId <= 0 || !card.deckName ||
      !card.front || !card.back || card.frontLength > MAX_SIDE_BYTES || card.backLength > MAX_SIDE_BYTES ||
      strlen(card.deckName) > MAX_DECK_NAME_BYTES || memchr(card.front, 0, card.frontLength) ||
      memchr(card.back, 0, card.backLength)) {
    LOG_ERR("ANKI", "Invalid card record");
    return false;
  }
  auto& state = *import_;
  for (uint16_t i = 0; i < state.count; ++i) {
    if (state.identities[i] == card.cardId) {
      LOG_ERR("ANKI", "Duplicate card ID");
      return false;
    }
  }
  uint16_t deck = 0;
  while (deck < state.decks && state.identities[state.expected + deck] != card.deckId) ++deck;
  // Reusable, at-most-256-byte stack record, not a retained deck collection.
  uint8_t record[DECK_BYTES] = {};
  if (deck == state.decks) {
    state.identities[state.expected + state.decks++] = card.deckId;
    put64(record, card.deckId);
    put16(record + 10, strlen(card.deckName));
    memcpy(record + 12, card.deckName, strlen(card.deckName));
  } else if (!state.file.seek(HEADER_BYTES + state.expected * CARD_BYTES + deck * DECK_BYTES) ||
             !exactRead(state.file, record, DECK_BYTES)) {
    LOG_ERR("ANKI", "Cannot read staged deck record");
    cancelSnapshot();
    return false;
  }
  put16(record + 8, get16(record + 8) + 1);
  if (!state.file.seek(HEADER_BYTES + state.expected * CARD_BYTES + deck * DECK_BYTES) ||
      state.file.write(record, DECK_BYTES) != DECK_BYTES) {
    LOG_ERR("ANKI", "Cannot write staged deck record");
    cancelSnapshot();
    return false;
  }
  memset(record, 0, CARD_BYTES);
  put64(record, card.cardId);
  put64(record + 8, card.deckId);
  put32(record + 16, state.end);
  put32(record + 20, card.frontLength);
  put32(record + 24, state.end + card.frontLength);
  put32(record + 28, card.backLength);
  put16(record + 32, deck);
  if (!state.file.seek(HEADER_BYTES + state.count * CARD_BYTES) || state.file.write(record, CARD_BYTES) != CARD_BYTES ||
      !state.file.seek(state.end) || state.file.write(card.front, card.frontLength) != card.frontLength ||
      state.file.write(card.back, card.backLength) != card.backLength) {
    LOG_ERR("ANKI", "Card snapshot write failed");
    // Deck counts/identity metadata may already have advanced. Discard the
    // import so retrying this card cannot publish partially updated indexes.
    cancelSnapshot();
    return false;
  }
  state.end += card.frontLength + card.backLength;
  state.identities[state.count++] = card.cardId;
  return true;
}

bool AnkiStore::commitSnapshot() {
  if (!import_ || import_->count != import_->expected) return false;
  auto& state = *import_;
  uint8_t header[HEADER_BYTES] = {};
  memcpy(header, "ANK1", 4);
  put16(header + 4, 1);
  put16(header + 6, state.count);
  put16(header + 8, state.decks);
  put16(header + 10, HEADER_BYTES);
  put32(header + 12, state.end);
  uint32_t crc = 0;
  if (!payloadCrc(state.file, crc)) {
    LOG_ERR("ANKI", "Cannot checksum snapshot");
    return false;
  }
  put32(header + 16, crc);
  header[28] = strlen(state.generation);
  memcpy(header + 32, state.generation, strlen(state.generation));
  put32(header + 124, crc32(header, 124) ^ 0xffffffffU);
  bool ok = state.file.seek(0) && state.file.write(header, sizeof(header)) == sizeof(header);
  ok = finishFile(state.file, ok);
  if (!ok || !validateSnapshot(STAGED, false)) return false;

  // Create the matching journal BEFORE publishing its content generation.
  char path[112];
  snprintf(path, sizeof(path), "%s/reviews-%016llx.log", ROOT,
           static_cast<unsigned long long>(generationHash(state.generation)));
  uint8_t journal[JOURNAL_HEADER_BYTES] = {};
  memcpy(journal, "ANJ1", 4);
  put16(journal + 4, JOURNAL_VERSION);
  journal[6] = strlen(state.generation);
  memcpy(journal + 16, state.generation, strlen(state.generation));
  put32(journal + 92, crc32(journal, 92) ^ 0xffffffffU);
  if (Storage.exists(path)) {
    // An interrupted activation can leave its empty journal behind.
    // Reuse only that exact header; never truncate an older generation's
    // reviews, a hash collision, or damaged evidence to make a retry succeed.
    auto log = Storage.open(path, O_RDWR);
    bool reusable = log && log.size() == sizeof(journal) && exactRead(log, header, sizeof(journal)) &&
                    memcmp(header, journal, sizeof(journal)) == 0;
    // The earlier attempt may have failed its sync. Verify durability again
    // before publishing a snapshot that depends on this otherwise empty file.
    if (log) reusable = finishFile(log, reusable);
    if (!reusable) {
      LOG_ERR("ANKI", "Generation journal already contains reviews or invalid data");
      return false;
    }
  } else {
    auto log = Storage.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!log || !finishFile(log, log.write(journal, sizeof(journal)) == sizeof(journal))) return false;
  }
  if (Storage.exists(BACKUP) && !Storage.remove(BACKUP)) return false;
  const bool old = Storage.exists(SNAPSHOT);
  if (old && !Storage.rename(SNAPSHOT, BACKUP)) return false;
  if (!Storage.rename(STAGED, SNAPSHOT)) {
    LOG_ERR("ANKI", "Cannot activate downloaded snapshot");
    if (old && !Storage.rename(BACKUP, SNAPSHOT)) failed_ = true;
    return false;
  }
  import_.reset();
  return open();
}

void AnkiStore::cancelSnapshot() {
  if (!import_) return;
  if (import_->file) import_->file.close();
  import_.reset();
  Storage.remove(STAGED);
}

bool AnkiStore::readCard(uint16_t ordinal, CardInfo& out) {
  if (failed_ || ordinal >= cardCount_) return false;
  auto file = Storage.open(SNAPSHOT, O_RDONLY);
  uint8_t record[CARD_BYTES];
  bool ok = file && file.seek(HEADER_BYTES + ordinal * CARD_BYTES) && exactRead(file, record, sizeof(record));
  if (file) ok = file.close() && ok;
  if (!ok) {
    LOG_ERR("ANKI", "Cannot read card index");
    failed_ = true;
    return false;
  }
  out = {static_cast<int64_t>(get64(record)), static_cast<int64_t>(get64(record + 8)), ordinal};
  if (out.cardId <= 0 || out.deckId <= 0) {
    LOG_ERR("ANKI", "Invalid card identity");
    failed_ = true;
    return false;
  }
  return true;
}
bool AnkiStore::readSide(uint16_t ordinal, bool answer, char* out, size_t capacity) {
  if (failed_ || ordinal >= cardCount_ || !out || !capacity) return false;
  auto file = Storage.open(SNAPSHOT, O_RDONLY);
  uint8_t record[CARD_BYTES];
  bool ok = file && file.seek(HEADER_BYTES + ordinal * CARD_BYTES) && exactRead(file, record, sizeof(record));
  const uint32_t offset = ok ? get32(record + (answer ? 24 : 16)) : 0;
  const uint32_t length = ok ? get32(record + (answer ? 28 : 20)) : 0;
  ok = ok && length <= MAX_SIDE_BYTES && length < capacity && offset <= file.size() && length <= file.size() - offset &&
       file.seek(offset) && exactRead(file, out, length);
  if (file) ok = file.close() && ok;
  if (!ok) {
    LOG_ERR("ANKI", "Cannot read card content");
    out[0] = 0;
    failed_ = true;
    return false;
  }
  out[length] = 0;
  return true;
}
bool AnkiStore::readDeck(uint16_t ordinal, Deck& out) {
  if (failed_ || ordinal >= deckCount_) return false;
  auto file = Storage.open(SNAPSHOT, O_RDONLY);
  uint8_t record[DECK_BYTES];
  bool ok = file && file.seek(HEADER_BYTES + cardCount_ * CARD_BYTES + ordinal * DECK_BYTES) &&
            exactRead(file, record, sizeof(record));
  if (!ok || get16(record + 10) > MAX_DECK_NAME_BYTES) {
    if (file) file.close();
    LOG_ERR("ANKI", "Invalid deck index");
    failed_ = true;
    return false;
  }
  out = {};
  out.deckId = static_cast<int64_t>(get64(record));
  out.cardCount = get16(record + 8);
  memcpy(out.name, record + 12, get16(record + 10));
  if (std::all_of(reviewed_.begin(), reviewed_.end(), [](uint8_t bits) { return bits == 0; })) {
    // A fresh snapshot or restarted pass needs no card-index scan per list row.
    out.remaining = out.cardCount;
  } else {
    ok = file.seek(HEADER_BYTES);
    for (uint16_t i = 0; ok && i < cardCount_; ++i) {
      ok = exactRead(file, record, CARD_BYTES);
      if (ok && static_cast<int64_t>(get64(record + 8)) == out.deckId && !(reviewed_[i / 8] & (1U << (i % 8))))
        ++out.remaining;
    }
  }
  if (file) ok = file.close() && ok;
  if (!ok) {
    LOG_ERR("ANKI", "Cannot count available cards");
    failed_ = true;
  }
  return ok;
}
bool AnkiStore::nextUnreviewed(int64_t deckId, uint16_t start, CardInfo& out) {
  if (failed_) return false;
  while (start < cardCount_ && (reviewed_[start / 8] & (1U << (start % 8)))) ++start;
  if (start >= cardCount_) return false;
  auto file = Storage.open(SNAPSHOT, O_RDONLY);
  uint8_t record[CARD_BYTES];
  bool ok = file && file.seek(HEADER_BYTES + start * CARD_BYTES), found = false;
  uint16_t nextOrdinal = start;
  for (uint16_t i = start; ok && i < cardCount_; ++i) {
    if (reviewed_[i / 8] & (1U << (i % 8))) continue;
    if (i != nextOrdinal) ok = file.seek(HEADER_BYTES + i * CARD_BYTES);
    ok = ok && exactRead(file, record, sizeof(record));
    nextOrdinal = i + 1;
    if (!ok) break;
    const int64_t cardId = static_cast<int64_t>(get64(record));
    const int64_t candidateDeckId = static_cast<int64_t>(get64(record + 8));
    if (cardId <= 0 || candidateDeckId <= 0) {
      ok = false;
    } else if (candidateDeckId == deckId) {
      out = {cardId, candidateDeckId, i};
      found = true;
      break;
    }
  }
  if (file) ok = file.close() && ok;
  if (!ok) {
    LOG_ERR("ANKI", "Cannot scan available cards");
    failed_ = true;
  }
  return ok && found;
}

void AnkiStore::resetReviewSchedule() {
  if (!reviewSchedule_) return;
  for (uint16_t i = 0; i < cardCount_; ++i) {
    if (reviewSchedule_[i] != OTHER_DECK) reviewSchedule_[i] = 0;
  }
  reviewTurn_ = 0;
  lastWasRetry_ = false;
}

void AnkiStore::applyReviewSchedule(const uint16_t ordinal, const uint8_t ease, const bool learning) {
  if (!reviewSchedule_ || reviewSchedule_[ordinal] == OTHER_DECK) return;
  lastWasRetry_ = reviewSchedule_[ordinal] != 0;
  ++reviewTurn_;
  reviewSchedule_[ordinal] = learning && ease <= 2 ? reviewTurn_ + (ease == 1 ? AGAIN_GAP : HARD_GAP) : 0;
}

bool AnkiStore::beginReview(const int64_t deckId) {
  reviewDeckId_ = 0;
  reviewTurn_ = 0;
  lastWasRetry_ = false;
  reviewSchedule_.reset();
  if (failed_ || attention_ || !hasSnapshot() || !cardCount_ || deckId <= 0) {
    LOG_ERR("ANKI", "Cannot initialize review queue in current state");
    return false;
  }
  // At most 4,000 bytes, allocated once per review rather than on the small
  // task stack or in every home/sync store. Reused without per-answer allocation.
  reviewSchedule_ = makeUniqueNoThrow<uint32_t[]>(cardCount_);
  if (!reviewSchedule_) {
    LOG_ERR("ANKI", "Cannot allocate %u-byte review schedule", static_cast<unsigned>(cardCount_ * sizeof(uint32_t)));
    return false;
  }
  auto snapshot = Storage.open(SNAPSHOT, O_RDONLY);
  // One 96-byte scratch buffer serves both indexes and journal records/header.
  uint8_t record[JOURNAL_HEADER_BYTES];
  bool ok = snapshot && snapshot.seek(HEADER_BYTES), found = false;
  for (uint16_t i = 0; ok && i < cardCount_; ++i) {
    ok = exactRead(snapshot, record, CARD_BYTES);
    if (!ok) break;
    const int64_t candidateDeck = static_cast<int64_t>(get64(record + 8));
    ok = static_cast<int64_t>(get64(record)) > 0 && candidateDeck > 0;
    reviewSchedule_[i] = candidateDeck == deckId ? 0 : OTHER_DECK;
    found = found || candidateDeck == deckId;
  }
  if (snapshot) ok = snapshot.close() && ok;
  if (ok && found) {
    char path[112];
    journalPath(path, sizeof(path));
    auto journal = Storage.open(path, O_RDONLY);
    ok = journal && exactRead(journal, record, sizeof(record)) && memcmp(record, "ANJ1", 4) == 0 &&
         get16(record + 4) == journalVersion_ && record[6] == strlen(generation_) &&
         memcmp(record + 16, generation_, strlen(generation_)) == 0 &&
         get32(record + 92) == (crc32(record, 92) ^ 0xffffffffU);
    uint32_t sequence = 1;
    while (ok && journal.position() < journal.size()) {
      ok = exactRead(journal, record, EVENT_BYTES) && memcmp(record, "AEV1", 4) == 0 &&
           get32(record + 60) == (crc32(record, 60) ^ 0xffffffffU);
      if (!ok) break;
      const uint8_t type = record[4];
      if (type == RATING || (type == LEARNING_RATING && journalVersion_ >= 3)) {
        const uint16_t ordinal = get16(record + 12);
        ok = get32(record + 8) == sequence++ && ordinal < cardCount_ && record[14] >= 1 && record[14] <= 4 &&
             reviewTurn_ < OTHER_DECK - HARD_GAP - 1;
        if (ok) {
          // Replay all ratings, even of now-completed cards: their answers
          // advanced this deck's spacing and alternation at the time.
          applyReviewSchedule(ordinal, record[14], type == LEARNING_RATING);
        }
      } else if (type == RESTART_DECK && journalVersion_ >= 2) {
        if (static_cast<int64_t>(get64(record + 48)) == deckId) resetReviewSchedule();
      } else if (type < PREPARED || type > RESOLVED) {
        ok = false;
      }
    }
    ok = ok && sequence == nextSequence_;
    if (journal) ok = journal.close() && ok;
  }
  if (!ok || !found) {
    LOG_ERR("ANKI", "Cannot restore review queue for deck %lld", static_cast<long long>(deckId));
    reviewSchedule_.reset();
    failed_ = true;
    return false;
  }
  reviewDeckId_ = deckId;
  return true;
}

bool AnkiStore::nextReviewCard(CardInfo& out) {
  if (failed_ || attention_ || !reviewSchedule_) {
    LOG_ERR("ANKI", "Review queue is unavailable");
    failed_ = true;
    return false;
  }
  uint16_t unseen = cardCount_, retry = cardCount_;
  for (uint16_t i = 0; i < cardCount_; ++i) {
    if (reviewSchedule_[i] == OTHER_DECK || (reviewed_[i / 8] & (1U << (i % 8)))) continue;
    if (reviewSchedule_[i] == 0) {
      if (unseen == cardCount_) unseen = i;
    } else if (retry == cardCount_ || reviewSchedule_[i] < reviewSchedule_[retry]) {
      retry = i;
    }
  }
  // Once ready, retries alternate with untouched cards. Earliest due wins
  // across both ratings; a small deck never waits for an artificial gap.
  const bool retryReady = retry < cardCount_ && reviewSchedule_[retry] <= reviewTurn_;
  const uint16_t selected = unseen == cardCount_ || (retryReady && !lastWasRetry_) ? retry : unseen;
  return selected < cardCount_ && readCard(selected, out);
}

bool AnkiStore::completedDeckMask(const int64_t deckId, ReviewedBits& mask) {
  mask.fill(0);
  if (deckId <= 0) return false;
  auto file = Storage.open(SNAPSHOT, O_RDONLY);
  uint8_t record[CARD_BYTES];
  bool ok = file && file.seek(HEADER_BYTES);
  bool found = false, complete = true;
  for (uint16_t i = 0; ok && i < cardCount_; ++i) {
    ok = exactRead(file, record, sizeof(record));
    if (ok && static_cast<int64_t>(get64(record + 8)) == deckId) {
      found = true;
      const uint8_t bit = 1U << (i % 8);
      mask[i / 8] |= bit;
      if (!(reviewed_[i / 8] & bit)) complete = false;
    }
  }
  if (file) ok = file.close() && ok;
  if (!ok) {
    LOG_ERR("ANKI", "Cannot read deck restart membership");
    failed_ = true;
  }
  return ok && found && complete;
}

bool AnkiStore::upgradeJournal() {
  if (journalVersion_ == JOURNAL_VERSION) return true;
  char path[112];
  journalPath(path, sizeof(path));
  auto file = Storage.open(path, O_RDONLY);
  const size_t bytes = file ? file.size() : 0;
  const bool closed = file && file.close();
  if (!closed || !repairJournal(bytes, true)) {
    LOG_ERR("ANKI", "Cannot upgrade review journal; reopen before continuing");
    failed_ = true;
    return false;
  }
  journalVersion_ = JOURNAL_VERSION;
  return true;
}

bool AnkiStore::restartDeck(const int64_t deckId) {
  if (failed_ || attention_ || !hasSnapshot() || nextSequence_ == std::numeric_limits<uint32_t>::max()) {
    LOG_ERR("ANKI", "Cannot restart deck in current storage/upload state");
    return false;
  }
  // 125 bytes of bounded scratch: reuse the retained bitmap and clear only
  // this deck after the restart event is durable, without another heap buffer.
  ReviewedBits mask{};
  if (!completedDeckMask(deckId, mask)) {
    LOG_ERR("ANKI", "Cannot restart missing or unfinished deck");
    return false;
  }
  if (!upgradeJournal() || !appendEvent(RESTART_DECK, nullptr, deckId)) return false;
  for (size_t i = 0; i < reviewed_.size(); ++i) reviewed_[i] &= ~mask[i];
  if (reviewDeckId_ == deckId) resetReviewSchedule();
  return true;
}

bool AnkiStore::appendEvent(uint8_t type, const Review* review, const int64_t deckId) {
  if (failed_ || !hasSnapshot()) return false;
  uint8_t bytes[EVENT_BYTES] = {};
  memcpy(bytes, "AEV1", 4);
  bytes[4] = type;
  if (review) {
    put32(bytes + 8, review->sequence);
    put16(bytes + 12, review->ordinal);
    bytes[14] = review->ease;
    put32(bytes + 16, review->durationMs);
    put64(bytes + 24, review->cardId);
    put64(bytes + 32, review->answeredAtMs);
  }
  put32(bytes + 40, preparedFirst_);
  bytes[44] = preparedCount_;
  put64(bytes + 48, deckId);
  put32(bytes + 60, crc32(bytes, 60) ^ 0xffffffffU);
  char path[112];
  journalPath(path, sizeof(path));
  auto file = Storage.open(path, O_WRONLY | O_APPEND);
  if (file && file.size() > std::numeric_limits<uint32_t>::max() - EVENT_BYTES) {
    file.close();
    LOG_ERR("ANKI", "Review journal exceeds supported file size");
    failed_ = true;
    return false;
  }
  if (!file || !finishFile(file, file.write(bytes, sizeof(bytes)) == sizeof(bytes))) {
    LOG_ERR("ANKI", "Review journal write failed; reopen before further reviews");
    failed_ = true;  // A partial append must be recovered before another write.
    return false;
  }
  return true;
}
bool AnkiStore::recordReview(uint16_t ordinal, uint8_t ease, uint32_t durationMs, uint64_t answeredAtMs) {
  if (failed_ || attention_ || ordinal >= cardCount_ || ease < 1 || ease > 4 || answeredAtMs > 9999999999999ULL ||
      nextSequence_ == std::numeric_limits<uint32_t>::max() || (reviewed_[ordinal / 8] & (1U << (ordinal % 8))) ||
      (reviewSchedule_ && reviewTurn_ >= OTHER_DECK - HARD_GAP - 1)) {
    LOG_ERR("ANKI", "Cannot record invalid, duplicate or blocked review");
    return false;
  }
  CardInfo card;
  if (!readCard(ordinal, card)) return false;
  Review review{nextSequence_, ordinal, card.cardId, ease, std::min(durationMs, uint32_t(3600000)), answeredAtMs};
  if (!upgradeJournal() || !appendEvent(LEARNING_RATING, &review)) return false;
  if (ease >= 3) reviewed_[ordinal / 8] |= 1U << (ordinal % 8);
  applyReviewSchedule(ordinal, ease, true);
  ++nextSequence_;
  ++pendingCount_;
  return true;
}

bool AnkiStore::repairJournal(size_t validBytes, const bool upgrade) {
  // Recovery runs once on entry. Bounded paths plus one 256-byte copy block
  // avoid allocating a second journal-sized buffer on the worker stack.
  char path[112], temp[124], damaged[128];
  journalPath(path, sizeof(path));
  snprintf(temp, sizeof(temp), "%s.tmp", path);
  snprintf(damaged, sizeof(damaged), "%s.damaged", path);
  // Keep the fixed .damaged name available for recovery of the rename window.
  // Earlier incomplete tails remain inspectable after another interrupted write.
  if (Storage.exists(damaged)) {
    char archived[144];
    bool moved = false;
    for (unsigned suffix = 1; suffix <= MAX_CARDS; ++suffix) {
      snprintf(archived, sizeof(archived), "%s.%u", damaged, suffix);
      if (Storage.exists(archived)) continue;
      moved = Storage.rename(damaged, archived);
      break;
    }
    if (!moved) {
      LOG_ERR("ANKI", "Could not preserve earlier damaged journal evidence");
      return false;
    }
  }
  auto source = Storage.open(path, O_RDONLY);
  auto destination = Storage.open(temp, O_WRONLY | O_CREAT | O_TRUNC);
  uint8_t buffer[256];
  bool ok = source && destination;
  bool first = true;
  while (ok && validBytes) {
    const size_t n = std::min(validBytes, sizeof(buffer));
    ok = exactRead(source, buffer, n);
    if (ok && first && upgrade) {
      if (n < JOURNAL_HEADER_BYTES) {
        ok = false;
      } else {
        put16(buffer + 4, JOURNAL_VERSION);
        put32(buffer + 92, crc32(buffer, 92) ^ 0xffffffffU);
      }
    }
    first = false;
    ok = ok && destination.write(buffer, n) == n;
    validBytes -= n;
  }
  if (source) ok = source.close() && ok;
  if (destination) ok = finishFile(destination, ok);
  if (!ok || !Storage.rename(path, damaged)) return false;
  if (!Storage.rename(temp, path)) {
    Storage.rename(damaged, path);
    return false;
  }
  reviewReadOffset_ = JOURNAL_HEADER_BYTES;
  reviewReadSequence_ = 1;
  LOG_INF("ANKI", "%s review journal; preserved original", upgrade ? "Upgraded" : "Repaired");
  return true;
}

bool AnkiStore::recoverJournal() {
  char path[112], damaged[128];
  journalPath(path, sizeof(path));
  snprintf(damaged, sizeof(damaged), "%s.damaged", path);
  // Finish an interrupted valid-prefix replacement by restoring the original;
  // do not assume a temporary prefix was synced merely because it exists.
  if (!Storage.exists(path) && Storage.exists(damaged) && !Storage.rename(damaged, path)) return false;
  auto file = Storage.open(path, O_RDONLY);
  uint8_t header[JOURNAL_HEADER_BYTES];
  bool ok = file && exactRead(file, header, sizeof(header)) && memcmp(header, "ANJ1", 4) == 0 &&
            get16(header + 4) >= 1 && get16(header + 4) <= JOURNAL_VERSION && header[6] == strlen(generation_) &&
            memcmp(header + 16, generation_, strlen(generation_)) == 0 &&
            get32(header + 92) == (crc32(header, 92) ^ 0xffffffffU);
  if (ok) journalVersion_ = get16(header + 4);
  size_t validBytes = JOURNAL_HEADER_BYTES;
  bool torn = false;
  uint8_t bytes[EVENT_BYTES];
  while (ok && file.position() < file.size()) {
    const size_t remaining = file.size() - file.position();
    if (remaining < EVENT_BYTES) {
      torn = true;
      break;
    }
    if (!exactRead(file, bytes, sizeof(bytes))) {
      ok = false;
      break;
    }
    if (memcmp(bytes, "AEV1", 4) || get32(bytes + 60) != (crc32(bytes, 60) ^ 0xffffffffU)) {
      // A complete-sized corrupt event might be an already durable attempt or
      // rating. Discarding it could replay an upload or present a rated card.
      ok = false;
      break;
    }
    const uint8_t type = bytes[4];
    if (type == RATING || (type == LEARNING_RATING && journalVersion_ >= 3)) {
      Review review;
      decodeReview(bytes, review);
      if (nextSequence_ == std::numeric_limits<uint32_t>::max() || review.sequence != nextSequence_ ||
          review.ordinal >= cardCount_ || review.ease < 1 || review.ease > 4 || review.durationMs > 3600000 ||
          review.answeredAtMs > 9999999999999ULL || review.cardId <= 0 ||
          (reviewed_[review.ordinal / 8] & (1U << (review.ordinal % 8)))) {
        ok = false;
        break;
      }
      if (type == RATING || review.ease >= 3) reviewed_[review.ordinal / 8] |= 1U << (review.ordinal % 8);
      ++nextSequence_;
    } else if (type == PREPARED) {
      if (preparedCount_ || get32(bytes + 40) != acknowledgedThrough_ + 1 || bytes[44] == 0 ||
          bytes[44] > MAX_UPLOAD_REVIEWS || get32(bytes + 40) > nextSequence_ ||
          bytes[44] > nextSequence_ - get32(bytes + 40)) {
        ok = false;
        break;
      }
      preparedFirst_ = get32(bytes + 40);
      preparedCount_ = bytes[44];
    } else if (type >= ATTEMPTED && type <= RESOLVED) {
      if (!preparedCount_ || preparedFirst_ != get32(bytes + 40) || preparedCount_ != bytes[44]) {
        ok = false;
        break;
      }
      if (type == ATTEMPTED || type == UNCERTAIN) {
        attempted_ = attention_ = true;
      } else {
        if (!attempted_) {
          ok = false;
          break;
        }
        acknowledgedThrough_ += preparedCount_;
        preparedFirst_ = preparedCount_ = 0;
        attempted_ = attention_ = false;
      }
    } else if (type == RESTART_DECK && journalVersion_ >= 2 && !attention_) {
      ReviewedBits mask{};
      if (!completedDeckMask(static_cast<int64_t>(get64(bytes + 48)), mask)) {
        ok = false;
        break;
      }
      for (size_t i = 0; i < reviewed_.size(); ++i) reviewed_[i] &= ~mask[i];
    } else {
      ok = false;
      break;
    }
    validBytes += EVENT_BYTES;
  }
  if (file) ok = file.close() && ok;
  if (!ok) {
    LOG_ERR("ANKI", "Journal integrity/state failure");
    return false;
  }
  if (torn && !repairJournal(validBytes)) return false;
  pendingCount_ = nextSequence_ - 1 - acknowledgedThrough_;
  return true;
}

bool AnkiStore::readReview(uint32_t sequence, Review& out) {
  char path[112];
  journalPath(path, sizeof(path));
  auto file = Storage.open(path, O_RDONLY);
  if (sequence < reviewReadSequence_) {
    reviewReadOffset_ = JOURNAL_HEADER_BYTES;
    reviewReadSequence_ = 1;
  }
  bool found = false;
  bool ok = file && file.seek(reviewReadOffset_);
  uint32_t nextOffset = reviewReadOffset_;
  uint8_t bytes[EVENT_BYTES];
  while (ok && file.position() < file.size()) {
    ok = exactRead(file, bytes, sizeof(bytes));
    ok = ok && memcmp(bytes, "AEV1", 4) == 0 && get32(bytes + 60) == (crc32(bytes, 60) ^ 0xffffffffU);
    if (ok && (bytes[4] == RATING || bytes[4] == LEARNING_RATING) && get32(bytes + 8) == sequence) {
      decodeReview(bytes, out);
      nextOffset = file.position();
      found = true;
      break;
    }
  }
  if (file) ok = file.close() && ok;
  if (!ok || !found) {
    LOG_ERR("ANKI", "Cannot read pending review");
    failed_ = true;
  } else {
    reviewReadOffset_ = nextOffset;
    reviewReadSequence_ = sequence + 1;
  }
  return ok && found;
}
bool AnkiStore::prepareUpload(UploadBatch& out) {
  if (failed_ || attention_ || !pendingCount_) return false;
  if (!preparedCount_) {
    preparedFirst_ = acknowledgedThrough_ + 1;
    preparedCount_ = std::min<uint32_t>(pendingCount_, MAX_UPLOAD_REVIEWS);
    if (!appendEvent(PREPARED)) return false;
  }
  out = {};
  batchId(out.batchId, sizeof(out.batchId));
  out.count = preparedCount_;
  for (uint8_t i = 0; i < out.count; ++i)
    if (!readReview(preparedFirst_ + i, out.reviews[i])) return false;
  return true;
}
bool AnkiStore::markUploadAttempted(const char* id) {
  char expected[MAX_BATCH_ID_BYTES + 1];
  batchId(expected, sizeof(expected));
  if (!preparedCount_ || attention_ || !id || strcmp(id, expected) || !appendEvent(ATTEMPTED)) return false;
  attempted_ = attention_ = true;
  return true;
}
bool AnkiStore::acknowledgeUpload(const char* id) {
  char expected[MAX_BATCH_ID_BYTES + 1];
  batchId(expected, sizeof(expected));
  if (!attempted_ || !id || strcmp(id, expected) || !appendEvent(ACKNOWLEDGED)) return false;
  acknowledgedThrough_ += preparedCount_;
  pendingCount_ -= preparedCount_;
  preparedFirst_ = preparedCount_ = 0;
  attempted_ = attention_ = false;
  return true;
}
bool AnkiStore::markUploadUncertain(const char* id) {
  char expected[MAX_BATCH_ID_BYTES + 1];
  batchId(expected, sizeof(expected));
  if (!attempted_ || !id || strcmp(id, expected)) return false;
  attention_ = true;  // ATTEMPTED already preserves this state even if the next append fails.
  return appendEvent(UNCERTAIN);
}
bool AnkiStore::exportRecovery() {
  if (failed_ || !attention_) return false;
  // Allocate one bounded Review on stack; report generation and batch identify
  // the original durable journal, which is retained even after resolution.
  char reportPath[144], id[MAX_BATCH_ID_BYTES + 1];
  batchId(id, sizeof(id));
  snprintf(reportPath, sizeof(reportPath), "%s/recovery-%s.ndjson", ROOT, id);
  auto report = Storage.open(reportPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!report) return false;
  bool ok = true;
  for (uint8_t i = 0; ok && i < preparedCount_; ++i) {
    Review review;
    ok = readReview(preparedFirst_ + i, review);
    char line[256];
    const int n =
        snprintf(line, sizeof(line),
                 "{\"batch_id\":\"%s\",\"card_id\":%lld,\"ease\":%u,\"duration_ms\":%lu,\"answered_at_ms\":%llu}\n", id,
                 static_cast<long long>(review.cardId), review.ease, static_cast<unsigned long>(review.durationMs),
                 static_cast<unsigned long long>(review.answeredAtMs));
    ok = ok && n > 0 && static_cast<size_t>(n) < sizeof(line) && report.write(line, n) == static_cast<size_t>(n);
  }
  return finishFile(report, ok);
}
bool AnkiStore::resolveAttention() {
  if (failed_ || !attention_ || !exportRecovery() || !appendEvent(RESOLVED)) return false;
  acknowledgedThrough_ += preparedCount_;
  pendingCount_ -= preparedCount_;
  preparedFirst_ = preparedCount_ = 0;
  attempted_ = attention_ = false;
  return true;
}
}  // namespace anki
#endif
