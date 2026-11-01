#include <gtest/gtest.h>

#include "AnkiStore.h"

namespace {
void checksum(std::vector<uint8_t>& bytes, const size_t offset, const size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = offset; i < offset + length; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  crc ^= 0xffffffffU;
  for (int i = 0; i < 4; ++i) bytes[offset + length + i] = crc >> (i * 8);
}

class StoreTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage = {}; }
  bool install(anki::AnkiStore& store, const char* generation = "generation-one", uint16_t count = 3) {
    if (!store.beginSnapshot(generation, count)) return false;
    for (uint16_t i = 0; i < count; ++i) {
      const anki::CardInput card{
          int64_t(100 + i), int64_t(i % 2 + 1), i % 2 ? "Second" : "First", "Question", "Answer", 8, 6};
      if (!store.appendCard(card)) return false;
    }
    return store.commitSnapshot();
  }
  std::string journal() const {
    for (const auto& [path, bytes] : Storage.files)
      if (path.ends_with(".log")) return path;
    return {};
  }
  void useVersionOneJournal() {
    auto& bytes = Storage.files[journal()];
    bytes[4] = 1;
    bytes[5] = 0;
    checksum(bytes, 0, 92);
  }
};
TEST_F(StoreTest, EmptyStoreAndSnapshotRoundTrip) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  EXPECT_FALSE(store.hasSnapshot());
  ASSERT_TRUE(install(store));
  EXPECT_EQ(store.cardCount(), 3);
  EXPECT_EQ(store.deckCount(), 2);
  anki::Deck deck;
  ASSERT_TRUE(store.readDeck(0, deck));
  EXPECT_STREQ(deck.name, "First");
  EXPECT_EQ(deck.remaining, 2);
  char side[32];
  ASSERT_TRUE(store.readSide(1, true, side, sizeof(side)));
  EXPECT_STREQ(side, "Answer");
  anki::AnkiStore restarted;
  ASSERT_TRUE(restarted.open());
  EXPECT_EQ(restarted.cardCount(), 3);
  for (const auto& [path, handles] : Storage.handles) EXPECT_EQ(handles, 0) << path;
}
TEST_F(StoreTest, EmptyBatchIsAValidSnapshot) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store, "empty", 0));
  EXPECT_TRUE(store.hasSnapshot());
  EXPECT_EQ(store.cardCount(), 0);
}
TEST_F(StoreTest, RejectsDuplicateCardsAndPreservesInstalledContent) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  ASSERT_TRUE(store.beginSnapshot("replacement", 2));
  anki::CardInput card{1, 2, "Deck", "F", "B", 1, 1};
  ASSERT_TRUE(store.appendCard(card));
  EXPECT_FALSE(store.appendCard(card));
  EXPECT_FALSE(store.commitSnapshot());
  store.cancelSnapshot();
  ASSERT_TRUE(store.open());
  EXPECT_STREQ(store.generation(), "generation-one");
}
TEST_F(StoreTest, RatingsSurviveAndCannotBeRecordedTwice) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  ASSERT_TRUE(store.recordReview(0, 1, 1234, 1790000000000ULL));
  EXPECT_FALSE(store.recordReview(0, 3, 1234));
  ASSERT_TRUE(store.open());
  EXPECT_EQ(store.pendingCount(), 1);
  anki::CardInfo next;
  ASSERT_TRUE(store.nextUnreviewed(1, 0, next));
  EXPECT_EQ(next.ordinal, 2);
  anki::UploadBatch batch;
  ASSERT_TRUE(store.prepareUpload(batch));
  EXPECT_EQ(batch.count, 1);
  EXPECT_EQ(batch.reviews[0].answeredAtMs, 1790000000000ULL);
}
TEST_F(StoreTest, UploadPreparationIsStableAndAttemptSurvivesReset) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  ASSERT_TRUE(store.recordReview(1, 4, 555));
  anki::UploadBatch first, second;
  ASSERT_TRUE(store.prepareUpload(first));
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(store.prepareUpload(second));
  EXPECT_STREQ(first.batchId, second.batchId);
  ASSERT_TRUE(store.markUploadAttempted(first.batchId));
  ASSERT_TRUE(store.open());
  EXPECT_TRUE(store.needsAttention());
  EXPECT_FALSE(store.prepareUpload(second));
  EXPECT_EQ(store.pendingCount(), 1);
  ASSERT_TRUE(store.exportRecovery());
  ASSERT_TRUE(store.resolveAttention());
  ASSERT_TRUE(store.open());
  EXPECT_FALSE(store.needsAttention());
  EXPECT_EQ(store.pendingCount(), 0);
  EXPECT_TRUE(Storage.exists(journal().c_str()));
}
TEST_F(StoreTest, AcknowledgedCardsStayReviewedWhenNextDownloadFails) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  ASSERT_TRUE(store.recordReview(0, 3, 1));
  anki::UploadBatch batch;
  ASSERT_TRUE(store.prepareUpload(batch));
  ASSERT_TRUE(store.markUploadAttempted(batch.batchId));
  ASSERT_TRUE(store.acknowledgeUpload(batch.batchId));
  ASSERT_TRUE(store.beginSnapshot("new", 1));
  store.cancelSnapshot();
  ASSERT_TRUE(store.open());
  EXPECT_EQ(store.pendingCount(), 0);
  anki::CardInfo next;
  ASSERT_TRUE(store.nextUnreviewed(1, 0, next));
  EXPECT_EQ(next.ordinal, 2);
}
TEST_F(StoreTest, PendingReviewsPreventReplacement) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  ASSERT_TRUE(store.recordReview(0, 3, 1));
  EXPECT_FALSE(store.beginSnapshot("new", 1));
}
TEST_F(StoreTest, RepairsTornTailBeforeAppendingAndKeepsEvidence) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  ASSERT_TRUE(store.recordReview(0, 3, 1));
  Storage.files[journal()].insert(Storage.files[journal()].end(), {1, 2, 3});
  ASSERT_TRUE(store.open());
  EXPECT_TRUE(Storage.exists((journal() + ".damaged").c_str()));
  ASSERT_TRUE(store.recordReview(1, 2, 42));
  ASSERT_TRUE(store.open());
  EXPECT_EQ(store.pendingCount(), 2);
}
TEST_F(StoreTest, CorruptionInsideJournalBlocksReviews) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  ASSERT_TRUE(store.recordReview(0, 3, 1));
  ASSERT_TRUE(store.recordReview(1, 3, 1));
  Storage.files[journal()][100] ^= 1;
  EXPECT_FALSE(store.open());
  EXPECT_TRUE(store.failed());
  EXPECT_FALSE(store.recordReview(2, 3, 1));
}
TEST_F(StoreTest, CorruptCompleteFinalEventIsNeverDiscardedAsAnIncompleteAppend) {
  for (const bool attempted : {false, true}) {
    Storage = {};
    anki::AnkiStore store;
    ASSERT_TRUE(store.open());
    ASSERT_TRUE(install(store));
    ASSERT_TRUE(store.recordReview(0, 3, 1));
    if (attempted) {
      anki::UploadBatch batch;
      ASSERT_TRUE(store.prepareUpload(batch));
      ASSERT_TRUE(store.markUploadAttempted(batch.batchId));
    }
    const auto path = journal();
    Storage.files[path].back() ^= 1;
    const auto damaged = Storage.files[path];
    EXPECT_FALSE(store.open());
    EXPECT_TRUE(store.failed());
    EXPECT_EQ(Storage.files[path], damaged);
    EXPECT_FALSE(Storage.exists((path + ".damaged").c_str()));
  }
}
TEST_F(StoreTest, RepeatedIncompleteAppendsPreserveAllPreviousEvidence) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  ASSERT_TRUE(store.recordReview(0, 3, 1));
  const auto path = journal();
  Storage.files[path].insert(Storage.files[path].end(), {1, 2, 3});
  const auto firstEvidence = Storage.files[path];
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(store.recordReview(1, 2, 42));
  Storage.files[path].insert(Storage.files[path].end(), {4, 5});
  ASSERT_TRUE(store.open());
  EXPECT_EQ(store.pendingCount(), 2);
  EXPECT_EQ(Storage.files[path + ".damaged.1"], firstEvidence);
  EXPECT_TRUE(Storage.exists((path + ".damaged").c_str()));
}
TEST_F(StoreTest, PendingReviewReadsVerifyTheJournalAgainBeforeUpload) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  ASSERT_TRUE(store.recordReview(0, 3, 1));
  anki::UploadBatch batch;
  ASSERT_TRUE(store.prepareUpload(batch));
  Storage.files[journal()][96 + 14] ^= 1;
  EXPECT_FALSE(store.prepareUpload(batch));
  EXPECT_TRUE(store.failed());
  EXPECT_FALSE(store.markUploadAttempted(batch.batchId));
}
TEST_F(StoreTest, IncompleteAcknowledgementKeepsTheUploadUncertain) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  ASSERT_TRUE(store.recordReview(0, 3, 1));
  anki::UploadBatch batch;
  ASSERT_TRUE(store.prepareUpload(batch));
  ASSERT_TRUE(store.markUploadAttempted(batch.batchId));
  ASSERT_TRUE(store.acknowledgeUpload(batch.batchId));
  auto& bytes = Storage.files[journal()];
  bytes.resize(bytes.size() - 20);
  ASSERT_TRUE(store.open());
  EXPECT_EQ(store.pendingCount(), 1);
  EXPECT_TRUE(store.needsAttention());
  EXPECT_FALSE(store.prepareUpload(batch));
}
TEST_F(StoreTest, MissingJournalNeverReintroducesCards) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  Storage.files.erase(journal());
  EXPECT_FALSE(store.open());
  EXPECT_TRUE(store.failed());
}
TEST_F(StoreTest, WriteSyncAndCloseFailuresDoNotAdvanceInMemory) {
  for (int mode = 0; mode < 3; ++mode) {
    Storage = {};
    anki::AnkiStore store;
    ASSERT_TRUE(store.open());
    ASSERT_TRUE(install(store));
    if (mode == 0) Storage.failWrite = true;
    if (mode == 1) Storage.failSync = true;
    // readCard closes first, so use operation-index fault injection separately for close.
    if (mode == 2) Storage.failAfter = 7;
    EXPECT_FALSE(store.recordReview(0, 3, 10));
    EXPECT_EQ(store.pendingCount(), 0);
  }
}
TEST_F(StoreTest, InterruptedActivationAlwaysPreservesOldOrCompleteNew) {
  for (int failure = 0; failure < 55; ++failure) {
    Storage = {};
    anki::AnkiStore store;
    ASSERT_TRUE(store.open());
    ASSERT_TRUE(install(store));
    ASSERT_TRUE(store.beginSnapshot("replacement", 1));
    anki::CardInput card{99, 3, "New", "F", "B", 1, 1};
    ASSERT_TRUE(store.appendCard(card));
    Storage.failAfter = failure;
    store.commitSnapshot();
    Storage.failAfter = -1;
    store.close();
    anki::AnkiStore restarted;
    ASSERT_TRUE(restarted.open()) << failure;
    EXPECT_TRUE(std::string(restarted.generation()) == "replacement" ||
                std::string(restarted.generation()) == "generation-one")
        << failure;
  }
}
TEST_F(StoreTest, EveryActivationCheckpointRetainsThePreviousReviewJournal) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  ASSERT_TRUE(store.recordReview(0, 3, 10));
  anki::UploadBatch batch;
  ASSERT_TRUE(store.prepareUpload(batch));
  ASSERT_TRUE(store.markUploadAttempted(batch.batchId));
  ASSERT_TRUE(store.acknowledgeUpload(batch.batchId));
  const auto oldJournal = journal();
  const auto oldBytes = Storage.files[oldJournal];
  ASSERT_TRUE(store.beginSnapshot("replacement", 1));
  const anki::CardInput card{99, 3, "New", "F", "B", 1, 1};
  ASSERT_TRUE(store.appendCard(card));
  Storage.captureCheckpoints = true;
  ASSERT_TRUE(store.commitSnapshot());
  Storage.captureCheckpoints = false;
  const auto checkpoints = std::move(Storage.checkpoints);
  ASSERT_FALSE(checkpoints.empty());
  for (size_t i = 0; i < checkpoints.size(); ++i) {
    SCOPED_TRACE(i);
    Storage.files = checkpoints[i];
    Storage.handles.clear();  // Simulate losing all open handles on reset.
    anki::AnkiStore restarted;
    ASSERT_TRUE(restarted.open());
    EXPECT_EQ(Storage.files[oldJournal], oldBytes);
    EXPECT_TRUE(std::string(restarted.generation()) == "replacement" ||
                std::string(restarted.generation()) == "generation-one");
    if (std::string(restarted.generation()) == "generation-one") {
      anki::CardInfo next;
      ASSERT_TRUE(restarted.nextUnreviewed(1, 0, next));
      EXPECT_EQ(next.ordinal, 2);
    }
  }
}
TEST_F(StoreTest, EveryAcknowledgementCheckpointIsEitherAcknowledgedOrUncertain) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  ASSERT_TRUE(store.recordReview(0, 3, 10));
  anki::UploadBatch batch;
  ASSERT_TRUE(store.prepareUpload(batch));
  ASSERT_TRUE(store.markUploadAttempted(batch.batchId));
  Storage.captureCheckpoints = true;
  ASSERT_TRUE(store.acknowledgeUpload(batch.batchId));
  Storage.captureCheckpoints = false;
  const auto checkpoints = std::move(Storage.checkpoints);
  for (const auto& checkpoint : checkpoints) {
    Storage.files = checkpoint;
    Storage.handles.clear();
    anki::AnkiStore restarted;
    ASSERT_TRUE(restarted.open());
    if (restarted.pendingCount()) {
      EXPECT_TRUE(restarted.needsAttention());
      EXPECT_FALSE(restarted.prepareUpload(batch));
    } else {
      EXPECT_FALSE(restarted.needsAttention());
    }
  }
}
TEST_F(StoreTest, EveryJournalRepairCheckpointKeepsAllAcceptedReviewsAndEarlierEvidence) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  ASSERT_TRUE(store.recordReview(0, 3, 10));
  const auto path = journal();
  Storage.files[path].push_back(1);
  const auto earlierEvidence = Storage.files[path];
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(store.recordReview(1, 2, 15));
  Storage.files[path].push_back(2);
  Storage.captureCheckpoints = true;
  ASSERT_TRUE(store.open());
  Storage.captureCheckpoints = false;
  const auto checkpoints = std::move(Storage.checkpoints);
  for (size_t i = 0; i < checkpoints.size(); ++i) {
    SCOPED_TRACE(i);
    Storage.files = checkpoints[i];
    Storage.handles.clear();
    anki::AnkiStore restarted;
    ASSERT_TRUE(restarted.open());
    EXPECT_EQ(restarted.pendingCount(), 2);
    EXPECT_EQ(Storage.files[path + ".damaged.1"], earlierEvidence);
    anki::CardInfo remaining;
    EXPECT_FALSE(restarted.nextUnreviewed(2, 0, remaining));
  }
}
TEST_F(StoreTest, MaximumDeckAndCardCountsPreserveLastOrdinalAndFullSide) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(store.beginSnapshot("maximum-snapshot", anki::MAX_CARDS));
  const std::string fullSide(anki::MAX_SIDE_BYTES, 'x');
  for (uint16_t i = 0; i < anki::MAX_CARDS; ++i) {
    const bool last = i + 1 == anki::MAX_CARDS;
    const anki::CardInput card{int64_t(10000 + i),
                               int64_t(20000 + i),
                               "Deck",
                               last ? fullSide.c_str() : "Q",
                               "A",
                               last ? fullSide.size() : size_t(1),
                               1};
    ASSERT_TRUE(store.appendCard(card)) << i;
  }
  ASSERT_TRUE(store.commitSnapshot());
  ASSERT_EQ(store.cardCount(), anki::MAX_CARDS);
  ASSERT_EQ(store.deckCount(), anki::MAX_DECKS);
  anki::Deck deck;
  ASSERT_TRUE(store.readDeck(anki::MAX_DECKS - 1, deck));
  EXPECT_EQ(deck.deckId, 20000 + anki::MAX_DECKS - 1);
  EXPECT_EQ(deck.remaining, 1);
  std::string side(anki::MAX_SIDE_BYTES + 1, '\0');
  ASSERT_TRUE(store.readSide(anki::MAX_CARDS - 1, false, side.data(), side.size()));
  EXPECT_EQ(std::string(side.c_str()), fullSide);
  ASSERT_TRUE(store.recordReview(anki::MAX_CARDS - 1, 4, 42));
  ASSERT_TRUE(store.open());
  ASSERT_EQ(store.pendingCount(), 1);
  anki::CardInfo next;
  EXPECT_FALSE(store.nextUnreviewed(deck.deckId, 0, next));
  EXPECT_FALSE(store.failed());
  anki::UploadBatch upload;
  ASSERT_TRUE(store.prepareUpload(upload));
  EXPECT_EQ(upload.reviews[0].ordinal, anki::MAX_CARDS - 1);
  EXPECT_EQ(upload.reviews[0].cardId, 10000 + anki::MAX_CARDS - 1);
}

TEST_F(StoreTest, BoundsAndCorruptionRejectWithoutReadingOutsideFiles) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  EXPECT_FALSE(store.beginSnapshot("bad/name", 1));
  EXPECT_FALSE(store.beginSnapshot("large", 1001));
  ASSERT_TRUE(install(store));
  Storage.files["/.crossink-anki/cards.bin"][4] = 9;
  EXPECT_FALSE(store.open());
}

TEST_F(StoreTest, RepeatsCompletedDecksIndependentlyAndResumesPartialPasses) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store));
  EXPECT_FALSE(store.restartDeck(0));
  EXPECT_FALSE(store.restartDeck(99));
  EXPECT_FALSE(store.restartDeck(1));
  ASSERT_TRUE(store.recordReview(0, 1, 10));
  EXPECT_FALSE(store.restartDeck(1));
  ASSERT_TRUE(store.recordReview(2, 2, 20));
  ASSERT_TRUE(store.recordReview(1, 3, 30));
  ASSERT_TRUE(store.restartDeck(1));
  ASSERT_TRUE(store.recordReview(0, 4, 40));
  EXPECT_FALSE(store.recordReview(0, 3, 50));
  ASSERT_TRUE(store.open());
  EXPECT_EQ(store.pendingCount(), 4U);
  anki::CardInfo card;
  ASSERT_TRUE(store.nextUnreviewed(1, 0, card));
  EXPECT_EQ(card.ordinal, 2);
  EXPECT_FALSE(store.nextUnreviewed(2, 0, card));
  EXPECT_FALSE(store.restartDeck(1));
  ASSERT_TRUE(store.recordReview(2, 3, 50));
  ASSERT_TRUE(store.restartDeck(2));
  ASSERT_TRUE(store.recordReview(1, 2, 60));
  ASSERT_TRUE(store.open());
  EXPECT_EQ(store.pendingCount(), 6U);
  anki::UploadBatch batch;
  ASSERT_TRUE(store.prepareUpload(batch));
  ASSERT_EQ(batch.count, 6);
  const int64_t ids[] = {100, 102, 101, 100, 102, 101};
  const uint8_t ratings[] = {1, 2, 3, 4, 3, 2};
  for (size_t i = 0; i < batch.count; ++i) {
    EXPECT_EQ(batch.reviews[i].sequence, i + 1);
    EXPECT_EQ(batch.reviews[i].cardId, ids[i]);
    EXPECT_EQ(batch.reviews[i].ease, ratings[i]);
    EXPECT_EQ(batch.reviews[i].durationMs, (i + 1) * 10);
  }
}

TEST_F(StoreTest, EmptyAndUncertainDecksCannotRestart) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  EXPECT_FALSE(store.restartDeck(1));
  ASSERT_TRUE(install(store, "empty", 0));
  EXPECT_FALSE(store.restartDeck(1));
  ASSERT_TRUE(install(store, "cards", 1));
  ASSERT_TRUE(store.recordReview(0, 3, 10));
  anki::UploadBatch batch;
  ASSERT_TRUE(store.prepareUpload(batch));
  ASSERT_TRUE(store.markUploadAttempted(batch.batchId));
  EXPECT_FALSE(store.restartDeck(1));
  ASSERT_TRUE(store.open());
  EXPECT_FALSE(store.restartDeck(1));
}

TEST_F(StoreTest, VersionOneUpgradeKeepsPreparedPayloadAndAcknowledgements) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store, "old", 1));
  ASSERT_TRUE(store.recordReview(0, 1, 123));
  anki::UploadBatch original, batch;
  ASSERT_TRUE(store.prepareUpload(original));
  useVersionOneJournal();
  const auto path = journal();
  const auto previous = Storage.files[path];
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(store.restartDeck(1));
  EXPECT_EQ(Storage.files[path][4], 2);
  EXPECT_EQ(Storage.files[path + ".damaged"], previous);
  ASSERT_TRUE(store.recordReview(0, 4, 456));
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(store.prepareUpload(batch));
  EXPECT_STREQ(batch.batchId, original.batchId);
  EXPECT_EQ(batch.count, original.count);
  EXPECT_EQ(batch.reviews[0].ease, 1);
  EXPECT_EQ(batch.reviews[0].durationMs, 123U);
  ASSERT_TRUE(store.markUploadAttempted(batch.batchId));
  ASSERT_TRUE(store.acknowledgeUpload(batch.batchId));
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(store.prepareUpload(batch));
  EXPECT_STRNE(batch.batchId, original.batchId);
  EXPECT_EQ(batch.count, 1);
  EXPECT_EQ(batch.reviews[0].sequence, 2U);
  EXPECT_EQ(batch.reviews[0].ease, 4);
  ASSERT_TRUE(store.markUploadAttempted(batch.batchId));
  ASSERT_TRUE(store.acknowledgeUpload(batch.batchId));
  ASSERT_TRUE(store.restartDeck(1));
  ASSERT_TRUE(store.open());
  EXPECT_EQ(store.pendingCount(), 0U);
  anki::CardInfo card;
  ASSERT_TRUE(store.nextUnreviewed(1, 0, card));
  EXPECT_EQ(card.ordinal, 0);
}

TEST_F(StoreTest, EveryUpgradeAndRestartCheckpointPreservesEarlierRatingsAndBatchIdentity) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store, "old", 1));
  ASSERT_TRUE(store.recordReview(0, 2, 123));
  anki::UploadBatch original;
  ASSERT_TRUE(store.prepareUpload(original));
  useVersionOneJournal();
  ASSERT_TRUE(store.open());
  Storage.captureCheckpoints = true;
  ASSERT_TRUE(store.restartDeck(1));
  Storage.captureCheckpoints = false;
  const auto checkpoints = std::move(Storage.checkpoints);
  ASSERT_FALSE(checkpoints.empty());
  for (size_t i = 0; i < checkpoints.size(); ++i) {
    SCOPED_TRACE(i);
    Storage.files = checkpoints[i];
    Storage.handles.clear();
    anki::AnkiStore restarted;
    ASSERT_TRUE(restarted.open());
    EXPECT_EQ(restarted.pendingCount(), 1U);
    anki::UploadBatch batch;
    ASSERT_TRUE(restarted.prepareUpload(batch));
    EXPECT_STREQ(batch.batchId, original.batchId);
    ASSERT_EQ(batch.count, 1);
    EXPECT_EQ(batch.reviews[0].ease, 2);
    EXPECT_EQ(batch.reviews[0].durationMs, 123U);
    anki::Deck deck;
    ASSERT_TRUE(restarted.readDeck(0, deck));
    EXPECT_LE(deck.remaining, 1);
    if (deck.remaining == 0) {
      ASSERT_TRUE(restarted.restartDeck(1));
    }
    ASSERT_TRUE(restarted.recordReview(0, 4, 321));
    ASSERT_TRUE(restarted.open());
    EXPECT_EQ(restarted.pendingCount(), 2U);
  }
}

TEST_F(StoreTest, RestartWriteFailuresDoNotChangeLiveProgress) {
  for (int mode = 0; mode < 3; ++mode) {
    SCOPED_TRACE(mode);
    Storage = {};
    anki::AnkiStore store;
    ASSERT_TRUE(store.open());
    ASSERT_TRUE(install(store, "cards", 1));
    ASSERT_TRUE(store.recordReview(0, 3, 10));
    if (mode == 0) Storage.failWrite = true;
    if (mode == 1) Storage.failSync = true;
    // Completed-deck scan is open, seek, read, close; append then opens/writes/syncs/closes.
    if (mode == 2) Storage.failAfter = 7;
    EXPECT_FALSE(store.restartDeck(1));
    EXPECT_EQ(store.pendingCount(), 1U);
    anki::CardInfo card;
    EXPECT_FALSE(store.nextUnreviewed(1, 0, card));
    ASSERT_TRUE(store.open());
    EXPECT_EQ(store.pendingCount(), 1U);
  }
}

TEST_F(StoreTest, TornRestartRetainsCompletedPassAndCorruptRestartBlocksRecovery) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store, "cards", 1));
  ASSERT_TRUE(store.recordReview(0, 3, 10));
  ASSERT_TRUE(store.restartDeck(1));
  const auto path = journal();
  Storage.files[path].resize(Storage.files[path].size() - 7);
  ASSERT_TRUE(store.open());
  EXPECT_EQ(store.pendingCount(), 1U);
  anki::CardInfo card;
  EXPECT_FALSE(store.nextUnreviewed(1, 0, card));
  ASSERT_TRUE(store.restartDeck(1));
  Storage.files[path].back() ^= 1;
  EXPECT_FALSE(store.open());
}

TEST_F(StoreTest, RestartWithUnknownDeckOrOldVersionIsRejectedEvenWithValidChecksum) {
  for (const bool oldVersion : {false, true}) {
    Storage = {};
    anki::AnkiStore store;
    ASSERT_TRUE(store.open());
    ASSERT_TRUE(install(store, "cards", 1));
    ASSERT_TRUE(store.recordReview(0, 3, 10));
    ASSERT_TRUE(store.restartDeck(1));
    if (oldVersion) {
      useVersionOneJournal();
    } else {
      auto& bytes = Storage.files[journal()];
      bytes[bytes.size() - 64 + 48] = 99;
      checksum(bytes, bytes.size() - 64, 60);
    }
    EXPECT_FALSE(store.open());
  }
}

TEST_F(StoreTest, RepeatedReviewsUploadSequentiallyWithoutRescanningEarlierBatches) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store, "cards", 40));
  for (int pass = 0; pass < 3; ++pass) {
    if (pass) {
      ASSERT_TRUE(store.restartDeck(1));
      ASSERT_TRUE(store.restartDeck(2));
    }
    for (uint16_t i = 0; i < 40; ++i) ASSERT_TRUE(store.recordReview(i, pass + 1, 10));
  }
  ASSERT_TRUE(store.open());
  Storage.bytesRead = 0;
  uint32_t seen = 0;
  anki::UploadBatch batch;
  while (store.pendingCount()) {
    ASSERT_TRUE(store.prepareUpload(batch));
    for (uint8_t i = 0; i < batch.count; ++i, ++seen) {
      EXPECT_EQ(batch.reviews[i].cardId, 100 + seen % 40);
      EXPECT_EQ(batch.reviews[i].ease, seen / 40 + 1);
    }
    ASSERT_TRUE(store.markUploadAttempted(batch.batchId));
    ASSERT_TRUE(store.acknowledgeUpload(batch.batchId));
  }
  EXPECT_EQ(seen, 120U);
  EXPECT_LE(Storage.bytesRead, 124U * 64U);
}

TEST_F(StoreTest, PendingCountSurvivesMoreThanSixtyFiveThousandReviews) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(install(store, "cards", 1));
  for (uint32_t i = 0; i < 65536; ++i) {
    if (i) {
      ASSERT_TRUE(store.restartDeck(1));
    }
    ASSERT_TRUE(store.recordReview(0, 3, 10));
  }
  EXPECT_EQ(store.pendingCount(), 65536U);
  ASSERT_TRUE(store.open());
  EXPECT_EQ(store.pendingCount(), 65536U);
  anki::UploadBatch batch;
  ASSERT_TRUE(store.prepareUpload(batch));
  EXPECT_EQ(batch.count, 32);
}
}  // namespace
