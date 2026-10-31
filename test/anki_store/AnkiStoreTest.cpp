#include <gtest/gtest.h>

#include "AnkiStore.h"

namespace {
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
TEST_F(StoreTest, BoundsAndCorruptionRejectWithoutReadingOutsideFiles) {
  anki::AnkiStore store;
  ASSERT_TRUE(store.open());
  EXPECT_FALSE(store.beginSnapshot("bad/name", 1));
  EXPECT_FALSE(store.beginSnapshot("large", 1001));
  ASSERT_TRUE(install(store));
  Storage.files["/.crossink-anki/cards.bin"][4] = 9;
  EXPECT_FALSE(store.open());
}
}  // namespace
