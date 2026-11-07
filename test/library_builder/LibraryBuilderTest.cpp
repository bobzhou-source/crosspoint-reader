#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "Epub.h"
#include "LibraryBuilder.h"
#include "LibraryIndexFile.h"

using namespace library;

namespace {

constexpr char INDEX[] = "/.crosspoint/library.idx";

std::string numbered(const char* prefix, const unsigned value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%s%04u", prefix, value);
  return text;
}

std::string pathAt(LibraryIndexFile& index, const SortOrder order, const uint16_t row) {
  const uint16_t ordinal = index.ordinalForRow(order, row);
  if (ordinal == 0xFFFF) return {};
  ClixRecord record{};
  if (!index.readRecord(ordinal, record)) return {};
  std::string path;
  return index.readPath(record, path) ? path : std::string();
}

class LibraryBuilderTest : public ::testing::Test {
 protected:
  BuildStats stats;

  void SetUp() override {
    fake::reset();
    bookMetadata.clear();
    fake::add("/a.epub");
    fake::add("/b.epub");
  }

  void initial() { ASSERT_TRUE(buildLibraryIndex("/", stats, true)); }
};

}  // namespace

TEST_F(LibraryBuilderTest, UnchangedRebuildReusesMetadataAndDoesNotReplaceIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.parsed, 0);
  EXPECT_EQ(stats.metadataReused, 2);
  EXPECT_FALSE(stats.indexReplaced);
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, FolderHeavyUnchangedReconciliationIoScalesLinearly) {
  const auto measure = [this](const unsigned count) {
    fake::reset();
    bookMetadata.clear();
    for (unsigned i = 0; i < count; i++) {
      fake::add("/folder" + numbered("", i) + "/book.txt");
    }
    if (!buildLibraryIndex("/", stats, false)) {
      ADD_FAILURE() << "initial build failed for " << count << " books";
      return 0u;
    }
    fake::resetIoCounters();
    if (!buildLibraryIndex("/", stats, false)) {
      ADD_FAILURE() << "unchanged build failed for " << count << " books";
      return 0u;
    }
    EXPECT_EQ(stats.metadataReused, count);
    EXPECT_FALSE(stats.indexReplaced);
    return fake::reads + fake::seeks;
  };

  const unsigned smallIo = measure(128);
  const unsigned largeIo = measure(256);
  EXPECT_LT(largeIo, smallIo * 3u);
}

TEST_F(LibraryBuilderTest, DirectoryEntriesAreEnumeratedOnce) {
  fake::add("/folder/c.txt");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_EQ(fake::directoryEntriesByPath["/a.epub"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/b.epub"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/folder"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/folder/c.txt"], 1u);
}

TEST_F(LibraryBuilderTest, DirectoryResumeFailureRetainsPreviousIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::add("/aa-folder/c.txt");
  fake::failDirectorySeek = true;

  EXPECT_FALSE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, StagingAndIndexWritesAreBatched) {
  fake::reset();
  for (unsigned i = 0; i < 128; i++) fake::add("/book" + numbered("", i) + ".txt");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_LT(fake::writesByPath["/.crosspoint/library.stage"], 64u);
  EXPECT_LT(fake::writesByPath["/.crosspoint/library.new"], 32u);
}

TEST_F(LibraryBuilderTest, ParentDuplicateTrackingSurvivesDirectoryRecursion) {
  fake::add("/folder/c.txt");
  fake::duplicateDirectoryEntry("/a.epub");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_EQ(stats.books, 3);
  EXPECT_EQ(stats.duplicatesDropped, 1);
}

TEST_F(LibraryBuilderTest, TimestampAndSizeChangesParseOnlyTheChangedBook) {
  initial();
  fake::files["/a.epub"]->time++;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);

  fake::files["/b.epub"]->bytes.push_back('x');
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);
}

TEST_F(LibraryBuilderTest, ZeroTimestampAndFailedExtractionAreNeverFresh) {
  fake::files["/a.epub"]->time = 0;
  bookMetadata["/b.epub"].success = false;
  initial();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.metadataReused, 0);
  EXPECT_TRUE(stats.indexReplaced);
}

TEST_F(LibraryBuilderTest, MetadataModeChangesInvalidateCachedMetadata) {
  initial();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().metadataEnabled, 0);
  index.close();

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 2u);
}

TEST_F(LibraryBuilderTest, RebuildVotesFromSourceAuthorInsteadOfPriorCanonicalAuthor) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"].author = "Victor Hugo";
  bookMetadata["/b.epub"].author = "Hugo Victor";
  bookMetadata["/c.epub"].author = "Hugo Victor";
  initial();
  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(Storage.remove("/c.epub"));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord record{};
  std::string author;
  ASSERT_TRUE(index.readRecord(0, record));
  ASSERT_TRUE(index.readAuthor(record, author));
  EXPECT_EQ(author, "Victor Hugo");
}

TEST_F(LibraryBuilderTest, EqualBasenamesInDifferentFoldersReconcileIndependently) {
  fake::add("/one/same.epub");
  fake::add("/two/same.epub");
  bookMetadata["/one/same.epub"].title = "One";
  bookMetadata["/two/same.epub"].title = "Two";
  initial();
  fake::files["/two/same.epub"]->time++;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 3);
}

TEST_F(LibraryBuilderTest, ArrivalOrderFollowsModificationTimeOverDiscoveryOrder) {
  // a and b exist with the default time; c lands with an older timestamp and d
  // with the newest, so file times, not walk or firstSeen order, decide.
  fake::add("/c.epub", "book c", /*time=*/0);
  fake::add("/d.epub", "book d", /*time=*/9);
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/c.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 3), "/d.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentDesc, 0), "/d.epub");
}

TEST_F(LibraryBuilderTest, AddedRemovedMovedAndRenamedBooksKeepArrivalOrder) {
  initial();
  fake::add("/c.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/c.epub");
  index.close();

  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(Storage.rename("/a.epub", "/moved.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.removed, 1);
  EXPECT_EQ(stats.renamed, 1);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/moved.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/c.epub");
  index.close();

  ASSERT_TRUE(Storage.rename("/moved.epub", "/renamed.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.renamed, 1);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/renamed.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/c.epub");
}

TEST_F(LibraryBuilderTest, WholeFolderRenameWithUniqueSizePreservesArrivalOrder) {
  fake::add("/old/unique.epub", "a uniquely sized book");
  initial();
  ASSERT_TRUE(Storage.mkdir("/new"));
  ASSERT_TRUE(Storage.rename("/old/unique.epub", "/new/unique.epub"));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(stats.renamed, 1);
  EXPECT_EQ(stats.removed, 0);
  EXPECT_EQ(fake::parses, 1u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/new/unique.epub");
}

TEST_F(LibraryBuilderTest, DuplicateDetectionRemainsBoundedAndFindsTrackedKeysAfterTheCap) {
  fake::duplicateDirectoryEntry("/a.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.books, 2);
  EXPECT_EQ(stats.duplicatesDropped, 1);
  EXPECT_FALSE(stats.dedupDegraded);

  fake::reset();
  bookMetadata.clear();
  for (unsigned i = 0; i <= LIBRARY_MAX_DEDUP_KEYS; i++) {
    fake::add("/book" + numbered("", i) + ".txt");
  }
  fake::duplicateDirectoryEntry("/book0000.txt");
  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(stats.books, LIBRARY_MAX_DEDUP_KEYS + 1);
  EXPECT_EQ(stats.duplicatesDropped, 1);
  EXPECT_TRUE(stats.dedupDegraded);
  EXPECT_LT(fake::delays, 2000u);
}

TEST_F(LibraryBuilderTest, ReadWriteCloseAndAllocationFailuresRetainPreviousIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;

  fake::failRead = 0;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failRead = -1;

  fake::files["/a.epub"]->time++;
  fake::failWrite = 0;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failWrite = -1;

  fake::failWritePath = "/.crosspoint/library.new";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);

  fake::failClosePath = "/.crosspoint/library.new";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);

  fake::failAlloc = 4;  // after the optional blob cache, fail the required prior table
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failAlloc = -1;

  fake::failRename = 1;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, TruncatedPersistedPathHashAbortsAndRetainsTheLiveIndex) {
  initial();
  auto& bytes = fake::files[INDEX]->bytes;
  ClixHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  ClixRecord record{};
  std::memcpy(&record, bytes.data() + recordOffset(header, 0), sizeof(record));
  record.nameOff = header.nameLen - 4;
  std::memcpy(bytes.data() + recordOffset(header, 0), &record, sizeof(record));
  const auto corrupted = bytes;

  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, corrupted);
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage.f"));
}

TEST_F(LibraryBuilderTest, LibrariesPastOldGateAndAtFormatCeilingKeepAllOrders) {
  for (const unsigned count : {513u, static_cast<unsigned>(CLIX_MAX_RECORDS)}) {
    fake::reset();
    bookMetadata.clear();
    std::vector<unsigned> authorOrder(count);
    std::iota(authorOrder.begin(), authorOrder.end(), 0u);
    for (unsigned i = 0; i < count; i++) {
      const std::string path = "/book" + numbered("", i) + ".epub";
      fake::add(path);
      bookMetadata[path].title = numbered("Title ", count - 1 - i);
      bookMetadata[path].author = numbered("Writer ", (i * (count == 513 ? 257u : 2053u)) % count);
    }

    ASSERT_TRUE(buildLibraryIndex("/", stats, true)) << count;
    ASSERT_EQ(stats.books, count);
    EXPECT_FALSE(stats.ranksDegraded);

    if (count == CLIX_MAX_RECORDS) {
      const auto old = fake::files[INDEX]->bytes;
      fake::parses = 0;
      fake::resetIoCounters();
      ASSERT_TRUE(buildLibraryIndex("/", stats, true));
      EXPECT_EQ(fake::parses, 0u);
      EXPECT_EQ(stats.metadataReused, CLIX_MAX_RECORDS);
      EXPECT_FALSE(stats.indexReplaced);
      EXPECT_EQ(fake::files[INDEX]->bytes, old);
      EXPECT_LT(fake::delays, 10000u);
    }

    std::sort(authorOrder.begin(), authorOrder.end(), [count](const unsigned a, const unsigned b) {
      return (a * (count == 513 ? 257u : 2053u)) % count < (b * (count == 513 ? 257u : 2053u)) % count;
    });
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    for (uint16_t row = 0; row < count; row++) {
      EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, row), "/book" + numbered("", row) + ".epub") << count << ':' << row;
      EXPECT_EQ(pathAt(index, SortOrder::TitleAsc, row), "/book" + numbered("", count - 1 - row) + ".epub")
          << count << ':' << row;
      EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, row), "/book" + numbered("", authorOrder[row]) + ".epub")
          << count << ':' << row;
    }
  }
}

TEST_F(LibraryBuilderTest, ASevenHundredBookShelfIndexesEveryFolderOnTheCard) {
  // The card as Robert's collection actually looks: loose books at the root, the
  // converted novel shelf (flat plus one publisher folder) and the local shelf
  // with a subfolder, next to the cloud inbox.
  fake::reset();
  bookMetadata.clear();
  std::vector<std::string> expected;
  const auto stage = [&expected](const std::string& path) {
    fake::add(path);
    expected.push_back(path);
  };
  for (unsigned i = 0; i < 6; i++) stage("/book" + numbered("", i) + ".epub");
  for (unsigned i = 0; i < 30; i++) stage("/books/Inbox/" + numbered("article", i) + ".epub");
  for (unsigned i = 0; i < 40; i++) stage("/books/Books/" + numbered("local", i) + ".epub");
  for (unsigned i = 0; i < 10; i++) stage("/books/Books/english/" + numbered("english", i) + ".epub");
  for (unsigned i = 0; i < 665; i++) stage("/books/Novels/" + numbered("novel", i) + ".epub");
  for (unsigned i = 0; i < 120; i++) stage("/books/Novels/Qidian/" + numbered("qidian", i) + ".epub");
  ASSERT_EQ(expected.size(), 871u);

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(stats.books, expected.size());
  EXPECT_EQ(stats.duplicatesDropped, 0);
  EXPECT_EQ(stats.unreadableSkipped, 0);
  EXPECT_FALSE(stats.dedupDegraded);
  EXPECT_FALSE(stats.ranksDegraded);
  EXPECT_EQ(stats.folders, 6);  // only folders that hold books: "/", Inbox, Books, Books/english, Novels, Novels/Qidian
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.bookCount(), expected.size());
  std::vector<std::string> seen;
  for (uint16_t row = 0; row < index.bookCount(); row++) seen.push_back(pathAt(index, SortOrder::RecentAsc, row));
  std::sort(seen.begin(), seen.end());
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(seen, expected);
}

TEST_F(LibraryBuilderTest, SortAllocationFailureProducesValidDegradedIndex) {
  fake::reset();
  for (unsigned i = 0; i < 513; i++) fake::add("/book" + numbered("", i) + ".txt");
  fake::failAlloc = 6;

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_TRUE(fake::failureTriggered);
  EXPECT_TRUE(stats.ranksDegraded);
  EXPECT_TRUE(stats.indexReplaced);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.bookCount(), 513);
}

TEST_F(LibraryBuilderTest, SeventeenHundredBooksPlusTwentyTwoOnlyParsesAdditions) {
  fake::reset();
  for (unsigned i = 0; i < 1700; i++) fake::add("/books/Inbox/" + numbered("article", i) + ".epub");
  initial();
  const auto old = fake::files[INDEX]->bytes;
  for (unsigned i = 1700; i < 1722; i++) fake::add("/books/Inbox/" + numbered("article", i) + ".epub");
  fake::parses = 0;
  BuildControl control;
  control.deferInstall = true;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, &control));
  EXPECT_EQ(stats.parsed, 22);
  EXPECT_EQ(stats.metadataReused, 1700);
  EXPECT_EQ(stats.books, 1722);
  EXPECT_TRUE(stats.indexPrepared);
  EXPECT_FALSE(stats.indexReplaced);
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  ASSERT_TRUE(installPreparedLibraryIndex());
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.bookCount(), 1722);
}

TEST_F(LibraryBuilderTest, InterruptedColdBuildResumesOnlyValidatedUnchangedMetadata) {
  fake::add("/c.epub");
  fake::add("/d.epub");
  BuildControl control;
  control.service = [](void*, uint16_t scanned, uint16_t, uint16_t) { return scanned < 3; };
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, &control));
  EXPECT_EQ(fake::parses, 3u);
  EXPECT_FALSE(Storage.exists(INDEX));
  ASSERT_TRUE(Storage.exists("/.crosspoint/library.resume"));
  EXPECT_TRUE(isLibraryIndexDirty());
  // A torn append cannot make the preceding complete entries unusable.
  auto& journal = fake::files["/.crosspoint/library.resume"]->bytes;
  journal.insert(journal.end(), 17, 0xCC);
  fake::files["/b.epub"]->time++;
  ASSERT_TRUE(Storage.remove("/c.epub"));
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.metadataResumed, 1);
  EXPECT_EQ(stats.parsed, 2);
  EXPECT_EQ(stats.books, 3);
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.resume"));
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  for (uint16_t row = 0; row < index.bookCount(); row++) EXPECT_NE(pathAt(index, SortOrder::TitleAsc, row), "/c.epub");
}

TEST_F(LibraryBuilderTest, CorruptCheckpointIsReparsedInsteadOfTrusted) {
  BuildControl control;
  control.service = [](void*, uint16_t scanned, uint16_t, uint16_t) { return scanned < 1; };
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, &control));
  fake::files["/.crosspoint/library.resume"]->bytes[30] ^= 1;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.parsed, 2);
  EXPECT_EQ(stats.metadataResumed, 0);
}

TEST_F(LibraryBuilderTest, OldFormatRebuildsCleanlyAndCurrentFormatRemainsReadable) {
  initial();
  fake::files[INDEX]->bytes[offsetof(ClixHeader, formatVersion)] = CLIX_FORMAT_VERSION - 1;
  LibraryIndexFile index;
  EXPECT_FALSE(index.open(INDEX));
  EXPECT_EQ(index.validity(), ClixValidity::UnknownFormatVersion);
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.bookCount(), 2);
  EXPECT_EQ(index.header().formatVersion, CLIX_FORMAT_VERSION);
}

TEST_F(LibraryBuilderTest, RemovingEveryBookPublishesEmptyShelfAndCountsRemovals) {
  initial();
  ASSERT_TRUE(Storage.remove("/a.epub"));
  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.removed, 2);
  EXPECT_EQ(stats.books, 0);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.bookCount(), 0);
}

TEST_F(LibraryBuilderTest, VisibleRowMetadataUsesOneBlobReadRegardlessOfLibrarySize) {
  bookMetadata["/a.epub"].title = "A title";
  bookMetadata["/a.epub"].author = "An Author";
  initial();
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord record{};
  ASSERT_TRUE(index.readRecord(0, record));
  fake::resetIoCounters();
  std::string text;
  ASSERT_TRUE(index.readAuthor(record, text));
  ASSERT_TRUE(index.readTitle(record, text));
  ASSERT_TRUE(index.readName(record, text));
  EXPECT_EQ(fake::reads, 1u);
  EXPECT_EQ(fake::seeks, 1u);
}

TEST_F(LibraryBuilderTest, TransferDuringBuildKeepsItsOwnDirtyGeneration) {
  initial();
  ASSERT_TRUE(markLibraryIndexDirty());
  bool transferred = false;
  BuildControl control;
  control.context = &transferred;
  control.service = [](void* context, uint16_t scanned, uint16_t, uint16_t) {
    bool& sent = *static_cast<bool*>(context);
    if (scanned == 1 && !sent) {
      sent = true;
      EXPECT_TRUE(markLibraryIndexDirty());
    }
    return true;
  };
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, &control));
  EXPECT_TRUE(transferred);
  EXPECT_TRUE(isLibraryIndexDirty());
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_FALSE(isLibraryIndexDirty());
}

TEST_F(LibraryBuilderTest, OversizedMetadataIsReparsedWithoutTruncatingAuthorIdentity) {
  bookMetadata["/a.epub"].author = std::string(130, 'A') + " Lastname";
  BuildControl control;
  control.service = [](void*, uint16_t scanned, uint16_t, uint16_t) { return scanned < 1; };
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, &control));
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.parsed, 2);
  EXPECT_EQ(stats.metadataResumed, 0);
}

TEST_F(LibraryBuilderTest, UnknownTimestampCannotReuseAnInterruptedCheckpoint) {
  fake::files["/a.epub"]->time = 0;
  BuildControl control;
  control.service = [](void*, uint16_t scanned, uint16_t, uint16_t) { return scanned < 1; };
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, &control));
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.parsed, 2);
  EXPECT_EQ(stats.metadataResumed, 0);
}

TEST_F(LibraryBuilderTest, RestartNeverRemovesTheOnlyPersistedRetryMarker) {
  BuildControl control;
  control.service = [](void*, uint16_t scanned, uint16_t, uint16_t) { return scanned < 1; };
  ASSERT_FALSE(buildLibraryIndex("/", stats, true, &control));
  const auto marker = fake::files["/.crosspoint/library.updating"];
  ASSERT_NE(marker, nullptr);
  control.service = [](void*, uint16_t, uint16_t, uint16_t) { return false; };
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, &control));
  EXPECT_EQ(fake::files["/.crosspoint/library.updating"], marker);
  EXPECT_TRUE(isLibraryIndexDirty());
}

TEST_F(LibraryBuilderTest, ShortBlobReadCannotPoisonThePreviousCachedRow) {
  bookMetadata["/a.epub"].title = "Alpha";
  bookMetadata["/b.epub"].title = "Bravo";
  initial();
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord first{};
  ClixRecord second{};
  ASSERT_TRUE(index.readRecord(0, first));
  ASSERT_TRUE(index.readRecord(1, second));
  std::string title;
  ASSERT_TRUE(index.readTitle(first, title));
  ASSERT_EQ(title, "Alpha");
  const auto bytes = fake::files[INDEX]->bytes;
  fake::files[INDEX]->bytes.pop_back();
  EXPECT_FALSE(index.readTitle(second, title));
  fake::files[INDEX]->bytes = bytes;
  ASSERT_TRUE(index.readTitle(first, title));
  EXPECT_EQ(title, "Alpha");
}
