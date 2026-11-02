#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "NavRestore.h"

using namespace benchmark::nav;
namespace R = benchmark::nav::restore;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes bytes(const std::string& text) { return Bytes(text.begin(), text.end()); }
Bytes filled(size_t n, uint8_t value) { return Bytes(n, value); }

std::string parentOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos || slash == 0 ? std::string("/") : path.substr(0, slash);
}

// An in-memory card with the failures the restore paths must survive.
class FakeFs : public RestoreFs {
 public:
  std::map<std::string, Bytes> files;
  std::set<std::string> dirs{"/", "/.crosspoint"};
  std::set<std::string> failWrite;       // writeFile fails
  std::set<std::string> failRemove;      // removeFile fails
  std::set<std::string> failRead;        // readAt fails
  std::set<std::string> corruptWrite;    // writeFile "succeeds" but stores different bytes
  bool failRename = false;
  int failRenameNth = 0;   // fails only the Nth rename call (1-based); 0 = off
  int failRenameFrom = 0;  // fails the Nth rename call and every one after it; 0 = off
  int renames = 0;
  bool failAppend = false;
  int writes = 0;
  size_t maxReadCap = 0;     // largest read buffer ever offered (the restore's memory bound)
  size_t maxWriteBytes = 0;  // largest single write or append

  void put(const std::string& path, const Bytes& data) {
    files[path] = data;
    addParents(path);
  }
  void put(const std::string& path, const std::string& text) { put(path, bytes(text)); }
  void addDir(const std::string& path) {
    dirs.insert(path);
    addParents(path);
  }
  const Bytes& get(const std::string& path) const { return files.at(path); }
  bool has(const std::string& path) const { return files.count(path) != 0; }

  bool exists(const char* path) override { return files.count(path) || dirs.count(path); }
  bool fileSize(const char* path, uint32_t& size) override {
    const auto it = files.find(path);
    if (it == files.end()) return false;
    size = static_cast<uint32_t>(it->second.size());
    return true;
  }
  bool readAt(const char* path, uint32_t offset, uint8_t* buf, size_t cap, size_t& n) override {
    n = 0;
    maxReadCap = std::max(maxReadCap, cap);
    const auto it = files.find(path);
    if (it == files.end() || failRead.count(path)) return false;
    if (offset >= it->second.size()) return true;
    n = std::min(cap, it->second.size() - offset);
    std::copy_n(it->second.begin() + offset, n, buf);
    return true;
  }
  bool writeFile(const char* path, const uint8_t* data, size_t n) override {
    ++writes;
    maxWriteBytes = std::max(maxWriteBytes, n);
    if (failWrite.count(path)) return false;
    Bytes stored(data, data + n);
    if (corruptWrite.count(path) && !stored.empty()) stored[0] ^= 0x5A;
    put(path, stored);
    return true;
  }
  bool appendFile(const char* path, const uint8_t* data, size_t n) override {
    maxWriteBytes = std::max(maxWriteBytes, n);
    if (failAppend) return false;
    auto& file = files[path];
    addParents(path);
    file.insert(file.end(), data, data + n);
    return true;
  }
  bool removeFile(const char* path) override {
    if (failRemove.count(path)) return false;
    files.erase(path);
    return true;
  }
  // FAT: a rename never overwrites and never deletes; it fails when the destination exists.
  bool renameFile(const char* from, const char* to) override {
    ++renames;
    if (failRename || renames == failRenameNth || (failRenameFrom && renames >= failRenameFrom)) return false;
    if (files.count(to)) return false;
    const auto it = files.find(from);
    if (it == files.end()) return false;
    files[to] = it->second;
    files.erase(from);
    addParents(to);
    return true;
  }
  bool makeDir(const char* path) override {
    addDir(path);
    return true;
  }
  bool listDir(const char* dir, const std::function<bool(const char*, bool)>& visit) override {
    if (!dirs.count(dir)) return false;
    const std::string prefix = std::string(dir) + "/";
    std::vector<std::pair<std::string, bool>> names;
    for (const auto& [path, data] : files)
      if (path.compare(0, prefix.size(), prefix) == 0 && path.find('/', prefix.size()) == std::string::npos)
        names.emplace_back(path.substr(prefix.size()), false);
    for (const auto& d : dirs)
      if (d.compare(0, prefix.size(), prefix) == 0 && d.find('/', prefix.size()) == std::string::npos)
        names.emplace_back(d.substr(prefix.size()), true);
    for (const auto& [name, isDir] : names)
      if (!visit(name.c_str(), isDir)) break;
    return true;
  }

 private:
  void addParents(const std::string& path) {
    for (std::string p = parentOf(path); p != "/" && !p.empty(); p = parentOf(p)) dirs.insert(p);
  }
};

// The owner's card: recents, state, settings, a statistics pair, three books with progress.
void seedCard(FakeFs& fs) {
  fs.put("/.crosspoint/recent.json", "{\"books\":[\"/Books/a.epub\",\"/Books/b.epub\"]}");
  fs.put("/.crosspoint/state.json", "{\"openEpubPath\":\"/Books/a.epub\"}");
  fs.put("/.crosspoint/settings.json", std::string(300, 's'));  // larger than the inline limit
  fs.put("/.crosspoint/reading-stats/daily.1", filled(200, 1));
  fs.put("/.crosspoint/reading-stats/00000000000000aa.0", filled(120, 2));
  fs.put("/.crosspoint/epub_111/progress.bin", Bytes{1, 0, 0, 0, 5, 0});
  fs.put("/.crosspoint/txt_222/progress.bin", Bytes{9, 9, 9, 9});
  fs.put("/.crosspoint/xtc_333/progress.bin", Bytes{7, 0, 0, 0});
  fs.put("/.crosspoint/epub_111/book.bin", filled(64, 3));  // cache, not state
  fs.put("/.crosspoint/wifi.json", "{\"ssid\":\"home\"}");  // outside a run's reach
  fs.put("/.crosspoint/annotations/progress.bin", Bytes{4});  // not a book cache dir
  fs.addDir("/.crosspoint/epub_444");                           // cached book without progress
}

// What a script that reads a fixture book and toggles settings leaves behind.
void mutateCard(FakeFs& fs) {
  fs.put("/.crosspoint/recent.json", "{\"books\":[\"/navfeat/probebook.epub\"]}");
  fs.put("/.crosspoint/state.json", "{\"openEpubPath\":\"/navfeat/probebook.epub\"}");
  fs.put("/.crosspoint/settings.json", std::string(300, 'N'));
  fs.put("/.crosspoint/reading-stats/daily.1", filled(200, 9));
  fs.put("/.crosspoint/reading-stats/daily.0", filled(200, 8));         // generation flipped
  fs.put("/.crosspoint/reading-stats/00000000000000bb.0", filled(90, 7));  // fixture book stats
  fs.put("/.crosspoint/epub_111/progress.bin", Bytes{2, 0, 0, 0, 9, 0});  // real book moved
  fs.files.erase("/.crosspoint/txt_222/progress.bin");                    // cleared
  fs.put("/.crosspoint/epub_555/progress.bin", Bytes{3, 0, 0, 0});        // fixture book cache
  fs.put("/.crosspoint/epub_444/progress.bin", Bytes{6, 0, 0, 0});        // had none
}



}  // namespace

TEST(NavRestoreCodec, Crc32MatchesTheStandardCheckValue) {
  const Bytes check = bytes("123456789");
  EXPECT_EQ(crc32(check.data(), check.size()), 0xCBF43926u);
  const uint32_t half = crc32(check.data(), 4);
  EXPECT_EQ(crc32(check.data() + 4, 5, half), 0xCBF43926u);
  EXPECT_EQ(crc32(nullptr, 0), 0u);
}

TEST(NavRestoreCodec, PathHashIsStableAndDistinguishesPaths) {
  EXPECT_EQ(pathHash(""), 2166136261u);
  EXPECT_NE(pathHash("/.crosspoint/recent.json"), pathHash("/.crosspoint/state.json"));
  EXPECT_EQ(pathHash("/a"), pathHash("/a"));
}

TEST(NavRestore, RoundTripPutsEveryTouchedFileBack) {
  FakeFs fs;
  seedCard(fs);
  const auto before = fs.files;
  const auto snap = snapshot(fs);
  ASSERT_TRUE(snap.ok());
  EXPECT_EQ(snap.entries, 4u + 2u + 3u);  // fixed + stats + progress of three real books
  EXPECT_TRUE(backupPending(fs));

  mutateCard(fs);
  const auto result = restoreSnapshot(fs);
  EXPECT_TRUE(result.found);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.failed, 0u);
  EXPECT_TRUE(result.discarded);
  EXPECT_EQ(result.attempted, snap.entries);

  for (const auto& [path, data] : before) {
    ASSERT_TRUE(fs.has(path)) << path;
    EXPECT_EQ(fs.get(path), data) << path;
  }
  // Nothing the run created survives, and the backup is gone.
  EXPECT_FALSE(fs.has("/.crosspoint/reading-stats/00000000000000bb.0"));
  EXPECT_FALSE(fs.has("/.crosspoint/reading-stats/daily.0"));
  EXPECT_FALSE(fs.has("/.crosspoint/epub_555/progress.bin"));
  EXPECT_FALSE(fs.has("/.crosspoint/epub_444/progress.bin"));
  EXPECT_EQ(result.extrasRemoved, 4u);  // bb stats, daily.0, epub_555 progress, epub_444 progress
  EXPECT_EQ(result.removed, 0u);        // everything recorded existed
  EXPECT_FALSE(backupPending(fs));
  for (const auto& [path, data] : fs.files) EXPECT_NE(path.rfind(R::BACKUP_DIR, 0), 0u) << path;
}

TEST(NavRestore, FilesOutsideTheRunsReachAreNeverTouched) {
  FakeFs fs;
  seedCard(fs);
  ASSERT_TRUE(snapshot(fs).ok());
  mutateCard(fs);
  fs.put("/.crosspoint/wifi.json", "{\"ssid\":\"changed\"}");
  fs.put("/.crosspoint/annotations/progress.bin", Bytes{99});
  fs.put("/.crosspoint/epub_111/book.bin", filled(64, 4));
  ASSERT_TRUE(restoreSnapshot(fs).ok());
  EXPECT_EQ(fs.get("/.crosspoint/wifi.json"), bytes("{\"ssid\":\"changed\"}"));
  EXPECT_EQ(fs.get("/.crosspoint/annotations/progress.bin"), Bytes{99});
  EXPECT_EQ(fs.get("/.crosspoint/epub_111/book.bin"), filled(64, 4));
}

TEST(NavRestore, AFileTheRunCreatedWhereNoneExistedIsDeleted) {
  FakeFs fs;
  fs.put("/.crosspoint/settings.json", "{}");
  ASSERT_TRUE(snapshot(fs).ok());
  fs.put("/.crosspoint/recent.json", "{\"books\":[\"x\"]}");
  fs.put("/.crosspoint/state.json", "{}");
  const auto result = restoreSnapshot(fs);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.removed, 2u);
  EXPECT_FALSE(fs.has("/.crosspoint/recent.json"));
  EXPECT_FALSE(fs.has("/.crosspoint/state.json"));
  EXPECT_EQ(fs.get("/.crosspoint/settings.json"), bytes("{}"));
}

TEST(NavRestore, EmptyFilesRoundTrip) {
  FakeFs fs;
  fs.put("/.crosspoint/recent.json", Bytes{});
  ASSERT_TRUE(snapshot(fs).ok());
  fs.put("/.crosspoint/recent.json", "now not empty");
  ASSERT_TRUE(restoreSnapshot(fs).ok());
  EXPECT_TRUE(fs.get("/.crosspoint/recent.json").empty());
}

TEST(NavRestore, NoManifestMeansNothingToRestore) {
  FakeFs fs;
  seedCard(fs);
  const auto before = fs.files;
  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.found);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.attempted, 0u);
  EXPECT_EQ(fs.files, before);
}

TEST(NavRestore, AFailedWriteIsReportedKeepsTheBackupAndRestoresTheRest) {
  FakeFs fs;
  seedCard(fs);
  const auto before = fs.files;
  ASSERT_TRUE(snapshot(fs).ok());
  mutateCard(fs);
  fs.failWrite.insert("/.crosspoint/settings.json");

  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.failed, 1u);
  EXPECT_EQ(result.firstFailedHash, pathHash("/.crosspoint/settings.json"));
  EXPECT_FALSE(result.discarded);
  EXPECT_TRUE(backupPending(fs));  // the next boot can retry
  EXPECT_EQ(fs.get("/.crosspoint/recent.json"), before.at("/.crosspoint/recent.json"));
  EXPECT_EQ(fs.get("/.crosspoint/epub_111/progress.bin"), before.at("/.crosspoint/epub_111/progress.bin"));
  EXPECT_EQ(fs.get("/.crosspoint/settings.json"), filled(300, 'N'));  // still the run's value

  fs.failWrite.clear();  // card recovers; the retry finishes the job
  const auto retry = restoreSnapshot(fs);
  EXPECT_TRUE(retry.ok());
  EXPECT_TRUE(retry.discarded);
  EXPECT_EQ(fs.get("/.crosspoint/settings.json"), before.at("/.crosspoint/settings.json"));
  EXPECT_FALSE(backupPending(fs));
}

TEST(NavRestore, ReadBackMismatchCountsAsAFailure) {
  FakeFs fs;
  seedCard(fs);
  ASSERT_TRUE(snapshot(fs).ok());
  mutateCard(fs);
  fs.corruptWrite.insert("/.crosspoint/recent.json");
  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.failed, 1u);
  EXPECT_EQ(result.firstFailedHash, pathHash("/.crosspoint/recent.json"));
  EXPECT_TRUE(backupPending(fs));
}

TEST(NavRestore, ADamagedBackupCopyFailsThatEntryOnly) {
  FakeFs fs;
  seedCard(fs);
  ASSERT_TRUE(snapshot(fs).ok());
  mutateCard(fs);
  // settings.json is entry 2 and spilled to its own data file.
  const std::string data = std::string(R::BACKUP_DIR) + "/d2.bin";
  ASSERT_TRUE(fs.has(data));
  fs.files[data][5] ^= 0xFF;
  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.failed, 1u);
  EXPECT_EQ(result.firstFailedHash, pathHash("/.crosspoint/settings.json"));
  EXPECT_EQ(fs.get("/.crosspoint/state.json"), bytes("{\"openEpubPath\":\"/Books/a.epub\"}"));
}

TEST(NavRestore, AMissingBackupCopyFailsThatEntry) {
  FakeFs fs;
  seedCard(fs);
  ASSERT_TRUE(snapshot(fs).ok());
  fs.files.erase(std::string(R::BACKUP_DIR) + "/d2.bin");
  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.failed, 1u);
  EXPECT_TRUE(backupPending(fs));
}

TEST(NavRestore, AFailedExtraDeletionIsReported) {
  FakeFs fs;
  seedCard(fs);
  ASSERT_TRUE(snapshot(fs).ok());
  mutateCard(fs);
  fs.failRemove.insert("/.crosspoint/epub_555/progress.bin");
  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.firstFailedHash, pathHash("/.crosspoint/epub_555/progress.bin"));
  EXPECT_TRUE(backupPending(fs));
}

TEST(NavRestore, AFailedDeletionOfAFileThatDidNotExistIsReported) {
  FakeFs fs;
  seedCard(fs);
  ASSERT_TRUE(snapshot(fs).ok());
  mutateCard(fs);
  fs.failRemove.insert("/.crosspoint/epub_444/progress.bin");
  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.firstFailedHash, pathHash("/.crosspoint/epub_444/progress.bin"));
}

TEST(NavRestore, AManifestThatCannotBeRemovedIsAFailure) {
  FakeFs fs;
  seedCard(fs);
  ASSERT_TRUE(snapshot(fs).ok());
  mutateCard(fs);
  fs.failRemove.insert(R::MANIFEST);
  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(result.discarded);
  EXPECT_EQ(result.firstFailedHash, pathHash(R::MANIFEST));
}

TEST(NavRestore, ADamagedManifestChangesNothing) {
  FakeFs fs;
  seedCard(fs);
  ASSERT_TRUE(snapshot(fs).ok());
  mutateCard(fs);
  const auto mutatedRecent = fs.get("/.crosspoint/recent.json");
  fs.files[R::MANIFEST][12] ^= 0x01;
  auto damaged = fs.files;
  const auto result = restoreSnapshot(fs);
  EXPECT_TRUE(result.found);
  EXPECT_FALSE(result.valid);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(fs.files, damaged);
  EXPECT_EQ(fs.get("/.crosspoint/recent.json"), mutatedRecent);
}

TEST(NavRestore, ATruncatedManifestChangesNothing) {
  FakeFs fs;
  seedCard(fs);
  ASSERT_TRUE(snapshot(fs).ok());
  mutateCard(fs);
  auto& manifest = fs.files[R::MANIFEST];
  manifest.resize(manifest.size() - 20);
  const auto before = fs.files;
  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.restored, 0u);
  EXPECT_EQ(fs.files, before);
}

TEST(NavRestore, AnUnreadableManifestIsAFailureNotACleanRun) {
  FakeFs fs;
  seedCard(fs);
  ASSERT_TRUE(snapshot(fs).ok());
  fs.failRead.insert(R::MANIFEST);
  const auto result = restoreSnapshot(fs);
  EXPECT_TRUE(result.found);
  EXPECT_FALSE(result.ok());
}

TEST(NavSnapshot, RefusesToOverwriteAnUnrestoredSnapshot) {
  FakeFs fs;
  seedCard(fs);
  ASSERT_TRUE(snapshot(fs).ok());
  mutateCard(fs);
  const auto second = snapshot(fs);
  EXPECT_EQ(second.error, SnapshotError::Pending);
  EXPECT_TRUE(restoreSnapshot(fs).ok());  // the original snapshot still restores
  EXPECT_EQ(fs.get("/.crosspoint/recent.json"), bytes("{\"books\":[\"/Books/a.epub\",\"/Books/b.epub\"]}"));
}

TEST(NavSnapshot, ARunCannotStartOverAFileTooLargeToProtect) {
  FakeFs fs;
  seedCard(fs);
  fs.put("/.crosspoint/recent.json", filled(R::MAX_FILE_BYTES + 1, 1));
  const auto result = snapshot(fs);
  EXPECT_EQ(result.error, SnapshotError::TooLarge);
  EXPECT_EQ(result.failedHash, pathHash("/.crosspoint/recent.json"));
  EXPECT_FALSE(backupPending(fs));
}

TEST(NavSnapshot, TooManyBooksIsRefusedNotTruncated) {
  FakeFs fs;
  for (uint32_t i = 0; i < R::MAX_ENTRIES; ++i)
    fs.put("/.crosspoint/epub_" + std::to_string(i) + "/progress.bin", Bytes{1});
  const auto result = snapshot(fs);
  EXPECT_EQ(result.error, SnapshotError::TooMany);
  EXPECT_FALSE(backupPending(fs));
}

TEST(NavSnapshot, AWriteFailureLeavesNoManifestSoNothingIsHalfProtected) {
  FakeFs fs;
  seedCard(fs);
  fs.failWrite.insert(std::string(R::BACKUP_DIR) + "/d2.bin");
  const auto result = snapshot(fs);
  EXPECT_EQ(result.error, SnapshotError::Io);
  EXPECT_FALSE(backupPending(fs));
}

TEST(NavSnapshot, AManifestRenameFailureLeavesNoManifest) {
  FakeFs fs;
  seedCard(fs);
  fs.failRename = true;
  EXPECT_EQ(snapshot(fs).error, SnapshotError::Io);
  EXPECT_FALSE(backupPending(fs));
  fs.failRename = false;
  // The orphans of the failed attempt are cleared by the next snapshot.
  const auto again = snapshot(fs);
  EXPECT_TRUE(again.ok());
  EXPECT_FALSE(fs.has(R::MANIFEST_TMP));
}

TEST(NavSnapshot, AnAppendFailureIsAnIoError) {
  FakeFs fs;
  seedCard(fs);
  fs.failAppend = true;
  EXPECT_EQ(snapshot(fs).error, SnapshotError::Io);
  EXPECT_FALSE(backupPending(fs));
}

TEST(NavSnapshot, AnUnreadableStateFileIsAnIoError) {
  FakeFs fs;
  seedCard(fs);
  fs.failRead.insert("/.crosspoint/state.json");
  const auto result = snapshot(fs);
  EXPECT_EQ(result.error, SnapshotError::Io);
  EXPECT_EQ(result.failedHash, pathHash("/.crosspoint/state.json"));
}

TEST(NavSnapshot, AnEmptyCardStillSnapshotsAndRestoresToEmpty) {
  FakeFs fs;
  const auto snap = snapshot(fs);
  ASSERT_TRUE(snap.ok());
  EXPECT_EQ(snap.entries, 4u);
  EXPECT_EQ(snap.present, 0u);
  fs.put("/.crosspoint/settings.json", "{}");
  EXPECT_TRUE(restoreSnapshot(fs).ok());
  EXPECT_FALSE(fs.has("/.crosspoint/settings.json"));
}

TEST(NavSnapshot, ReaderSyncStatusWrittenByARealAttemptIsRestoredOrRemoved) {
  FakeFs fs;
  fs.put("/.crosspoint/reader-sync/status.json", "{\"last\":1}");
  const auto snap = snapshot(fs);
  ASSERT_TRUE(snap.ok());
  fs.put("/.crosspoint/reader-sync/status.json", "{\"last\":2,\"phase\":6}");  // the offline attempt persisted
  EXPECT_TRUE(restoreSnapshot(fs).ok());
  EXPECT_EQ(fs.get("/.crosspoint/reader-sync/status.json"), bytes("{\"last\":1}"));

  FakeFs fresh;  // no status file before the run: the attempt's file is removed
  ASSERT_TRUE(snapshot(fresh).ok());
  fresh.put("/.crosspoint/reader-sync/status.json", "{\"last\":2}");
  EXPECT_TRUE(restoreSnapshot(fresh).ok());
  EXPECT_FALSE(fresh.has("/.crosspoint/reader-sync/status.json"));
}

TEST(NavSnapshot, LongPathsStayWithinTheManifestLineBound) {
  FakeFs fs;
  const std::string longName(R::MAX_LINE, 'x');
  fs.put("/.crosspoint/reading-stats/" + longName, Bytes{1});
  const auto result = snapshot(fs);
  EXPECT_EQ(result.error, SnapshotError::Io);
  EXPECT_FALSE(backupPending(fs));
}

namespace {

std::string cacheDir(const char* prefix, int i) {
  char name[32];
  snprintf(name, sizeof(name), "%s%04d", prefix, i);
  return std::string("/.crosspoint/") + name;
}

Bytes patterned(size_t n, uint8_t salt) {
  Bytes data(n);
  for (size_t i = 0; i < n; ++i) data[i] = static_cast<uint8_t>(i * 7 + (i >> 8) * 13 + salt);
  return data;
}

bool backupDirHoldsFiles(const FakeFs& fs) {
  const std::string prefix = std::string(R::BACKUP_DIR) + "/";
  for (const auto& [path, data] : fs.files)
    if (path.compare(0, prefix.size(), prefix) == 0) return true;
  return false;
}

// `count` cached books with a real progress file each, named so they list in index order.
void seedBooksWithProgress(FakeFs& fs, int count) {
  for (int i = 0; i < count; ++i) {
    fs.put(cacheDir("epub_", i) + "/progress.bin", Bytes{static_cast<uint8_t>(i % 251), 1});
    fs.put(cacheDir("epub_", i) + "/book.bin", Bytes{2});
  }
}

}  // namespace

TEST(NavSnapshotLargeCard, ProgressFilesAtTheTailOfSeventeenHundredFiftyCachesAreProtectedAndRestored) {
  FakeFs fs;
  seedCard(fs);
  // A card with 1750 cached books of which only the last few have a reading position: the cache folders alone exceed
  // the entry cap, yet the files that matter sit at the very end of the listing.
  for (int i = 0; i < 1750; ++i) fs.put(cacheDir("epub_", i) + "/book.bin", Bytes{2});
  const int tail[] = {1740, 1741, 1742, 1743, 1744, 1745};
  for (int i : tail) fs.put(cacheDir("epub_", i) + "/progress.bin", Bytes{static_cast<uint8_t>(i % 251), 5, 5});
  const auto before = fs.files;

  const auto snap = snapshot(fs);
  ASSERT_TRUE(snap.ok());
  EXPECT_EQ(snap.entries, 4u + 2u + 3u + 6u);  // fixed, stats, the three seeded books, the six tail books
  EXPECT_TRUE(backupPending(fs));

  mutateCard(fs);
  fs.put(cacheDir("epub_", 1741) + "/progress.bin", Bytes{99});
  fs.files.erase(cacheDir("epub_", 1742) + "/progress.bin");
  fs.put(cacheDir("epub_", 1748) + "/progress.bin", Bytes{7});  // fixture book the run opened
  fs.put(cacheDir("epub_", 1749) + "/progress.bin", Bytes{8});

  const auto result = restoreSnapshot(fs);
  EXPECT_TRUE(result.ok());
  EXPECT_TRUE(result.discarded);
  EXPECT_EQ(result.attempted, snap.entries);
  EXPECT_EQ(result.extrasRemoved, 4u + 2u);  // the four from mutateCard, the two late ones
  for (const auto& [path, data] : before) {
    ASSERT_TRUE(fs.has(path)) << path;
    EXPECT_EQ(fs.get(path), data) << path;
  }
  EXPECT_FALSE(fs.has(cacheDir("epub_", 1748) + "/progress.bin"));
  EXPECT_FALSE(fs.has(cacheDir("epub_", 1749) + "/progress.bin"));
  EXPECT_FALSE(backupDirHoldsFiles(fs));
  EXPECT_LE(fs.maxReadCap, 1024u);  // bounded buffers whatever the card holds
  EXPECT_LE(fs.maxWriteBytes, 1024u);
}

TEST(NavSnapshotLargeCard, AProgressFileThatSitsPastTheFirstThousandFiveHundredFoldersIsNeverMissed) {
  FakeFs fs;
  for (int i = 0; i < 1700; ++i) fs.addDir(cacheDir("txt_", i));
  fs.put(cacheDir("txt_", 1699) + "/progress.bin", Bytes{4, 2});
  const auto snap = snapshot(fs);
  ASSERT_TRUE(snap.ok());
  EXPECT_EQ(snap.entries, 4u + 1u);
  fs.put(cacheDir("txt_", 1699) + "/progress.bin", Bytes{9});
  fs.put(cacheDir("txt_", 1698) + "/progress.bin", Bytes{9});  // created by the run, listed before it
  const auto result = restoreSnapshot(fs);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.extrasRemoved, 1u);
  EXPECT_EQ(fs.get(cacheDir("txt_", 1699) + "/progress.bin"), (Bytes{4, 2}));
  EXPECT_FALSE(fs.has(cacheDir("txt_", 1698) + "/progress.bin"));
}

TEST(NavSnapshotLargeCard, TheEntryCapIsExactAndAnythingOverItIsRefusedBeforeTheRun) {
  {
    FakeFs fs;  // 4 fixed files + 1532 progress files == the cap
    seedBooksWithProgress(fs, static_cast<int>(R::MAX_ENTRIES) - 4);
    const auto snap = snapshot(fs);
    ASSERT_TRUE(snap.ok());
    EXPECT_EQ(snap.entries, R::MAX_ENTRIES);
    fs.put(cacheDir("epub_", 0) + "/progress.bin", Bytes{200});
    fs.put(cacheDir("epub_", 1531) + "/progress.bin", Bytes{201});
    const auto result = restoreSnapshot(fs);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.attempted, R::MAX_ENTRIES);
    EXPECT_EQ(fs.get(cacheDir("epub_", 0) + "/progress.bin"), (Bytes{0, 1}));
    EXPECT_EQ(fs.get(cacheDir("epub_", 1531) + "/progress.bin"), (Bytes{static_cast<uint8_t>(1531 % 251), 1}));
  }
  {
    FakeFs fs;  // one more, found at the very end of the listing
    seedBooksWithProgress(fs, static_cast<int>(R::MAX_ENTRIES) - 3);
    const auto before = fs.files;
    const auto snap = snapshot(fs);
    EXPECT_EQ(snap.error, SnapshotError::TooMany);
    EXPECT_EQ(snap.failedHash, pathHash((cacheDir("epub_", 1532) + "/progress.bin").c_str()));
    EXPECT_FALSE(backupPending(fs));
    EXPECT_FALSE(backupDirHoldsFiles(fs));  // nothing half-copied is left behind
    EXPECT_EQ(fs.files, before);            // and the card itself is untouched
  }
  {
    FakeFs fs;  // a realistic oversized card
    seedBooksWithProgress(fs, 1700);
    EXPECT_EQ(snapshot(fs).error, SnapshotError::TooMany);
    EXPECT_FALSE(backupPending(fs));
    EXPECT_FALSE(backupDirHoldsFiles(fs));
  }
}

TEST(NavSnapshotLargeCard, StatisticsFilesCountTowardsTheCapToo) {
  FakeFs fs;
  seedBooksWithProgress(fs, static_cast<int>(R::MAX_ENTRIES) - 4 - 2);
  fs.put("/.crosspoint/reading-stats/a", Bytes{1});
  fs.put("/.crosspoint/reading-stats/b", Bytes{1});
  EXPECT_TRUE(snapshot(fs).ok());
  ASSERT_TRUE(restoreSnapshot(fs).ok());
  fs.put("/.crosspoint/reading-stats/c", Bytes{1});
  EXPECT_EQ(snapshot(fs).error, SnapshotError::TooMany);
  EXPECT_FALSE(backupPending(fs));
}

TEST(NavSnapshotLargeFile, AFileOfExactlyTheLimitRoundTripsThroughBoundedBuffers) {
  FakeFs fs;
  seedCard(fs);
  const Bytes biggest = patterned(R::MAX_FILE_BYTES, 1);
  fs.put("/.crosspoint/recent.json", biggest);
  fs.put("/.crosspoint/reading-stats/daily.1", patterned(20000, 3));
  const auto before = fs.files;

  const auto snap = snapshot(fs);
  ASSERT_TRUE(snap.ok());
  mutateCard(fs);
  fs.put("/.crosspoint/recent.json", patterned(R::MAX_FILE_BYTES, 2));

  const auto result = restoreSnapshot(fs);
  EXPECT_TRUE(result.ok());
  EXPECT_TRUE(result.discarded);
  EXPECT_EQ(fs.get("/.crosspoint/recent.json"), biggest);
  for (const auto& [path, data] : before) {
    ASSERT_TRUE(fs.has(path)) << path;
    EXPECT_EQ(fs.get(path), data) << path;
  }
  EXPECT_LE(fs.maxReadCap, 1024u);  // never a 32 KiB buffer
  EXPECT_LE(fs.maxWriteBytes, 1024u);
  for (const auto& [path, data] : fs.files) {
    EXPECT_EQ(path.find(".nvtmp"), std::string::npos) << path;
    EXPECT_NE(path.rfind(R::BACKUP_DIR, 0), 0u) << path;
  }
}

TEST(NavSnapshotLargeFile, OneByteOverTheLimitIsRefusedBeforeTheRunAndLeavesNoBackup) {
  FakeFs fs;
  seedCard(fs);
  fs.put("/.crosspoint/reading-stats/daily.1", patterned(R::MAX_FILE_BYTES + 1, 1));
  const auto before = fs.files;
  const auto result = snapshot(fs);
  EXPECT_EQ(result.error, SnapshotError::TooLarge);
  EXPECT_EQ(result.failedHash, pathHash("/.crosspoint/reading-stats/daily.1"));
  EXPECT_FALSE(backupPending(fs));
  EXPECT_FALSE(backupDirHoldsFiles(fs));
  EXPECT_EQ(fs.files, before);
}

TEST(NavRestoreLargeFile, AFailedTemporaryWriteKeepsTheBackupAndTheDestinationUntouched) {
  FakeFs fs;
  seedCard(fs);
  const Bytes original = patterned(9000, 1);
  fs.put("/.crosspoint/reading-stats/daily.1", original);
  ASSERT_TRUE(snapshot(fs).ok());
  const Bytes during = patterned(9000, 2);
  fs.put("/.crosspoint/reading-stats/daily.1", during);

  fs.failWrite.insert("/.crosspoint/reading-stats/daily.1.nvtmp");
  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.failed, 1u);
  EXPECT_EQ(result.firstFailedHash, pathHash("/.crosspoint/reading-stats/daily.1"));
  EXPECT_FALSE(result.discarded);
  EXPECT_TRUE(backupPending(fs));
  EXPECT_TRUE(backupDirHoldsFiles(fs));
  EXPECT_EQ(fs.get("/.crosspoint/reading-stats/daily.1"), during);  // never half written

  fs.failWrite.clear();
  const auto retry = restoreSnapshot(fs);
  EXPECT_TRUE(retry.ok());
  EXPECT_EQ(fs.get("/.crosspoint/reading-stats/daily.1"), original);
  EXPECT_FALSE(backupPending(fs));
}

TEST(NavRestoreLargeFile, AFailedRenameCleansTheTemporaryAndKeepsTheBackup) {
  FakeFs fs;
  seedCard(fs);
  fs.put("/.crosspoint/reading-stats/daily.1", patterned(9000, 1));
  ASSERT_TRUE(snapshot(fs).ok());
  const Bytes during = patterned(9000, 2);
  fs.put("/.crosspoint/reading-stats/daily.1", during);
  fs.failRename = true;
  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(backupPending(fs));
  EXPECT_FALSE(fs.has("/.crosspoint/reading-stats/daily.1.nvtmp"));
  EXPECT_EQ(fs.get("/.crosspoint/reading-stats/daily.1"), during);
}

TEST(NavRestoreLargeFile, ADamagedLaterChunkOfTheBackupCopyIsCaughtBeforeTheDestinationIsTouched) {
  FakeFs fs;
  seedCard(fs);
  fs.put("/.crosspoint/reading-stats/daily.1", patterned(9000, 1));
  ASSERT_TRUE(snapshot(fs).ok());
  const Bytes during = patterned(9000, 2);
  fs.put("/.crosspoint/reading-stats/daily.1", during);
  const std::string copy = std::string(R::BACKUP_DIR) + "/d5.bin";  // fixed 0-3, the 00..aa stats file, then daily.1
  ASSERT_TRUE(fs.has(copy));
  ASSERT_EQ(fs.get(copy).size(), 9000u);
  fs.files[copy][8500] ^= 0x40;
  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.failed, 1u);
  EXPECT_EQ(fs.get("/.crosspoint/reading-stats/daily.1"), during);
  EXPECT_TRUE(backupPending(fs));
}

TEST(NavRestoreLargeFile, AReadBackMismatchOfAMultiChunkFileIsAFailure) {
  FakeFs fs;
  seedCard(fs);
  const Bytes original = patterned(9000, 1);
  fs.put("/.crosspoint/reading-stats/daily.1", original);
  ASSERT_TRUE(snapshot(fs).ok());
  fs.put("/.crosspoint/reading-stats/daily.1", patterned(9000, 2));
  fs.corruptWrite.insert("/.crosspoint/reading-stats/daily.1.nvtmp");
  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.firstFailedHash, pathHash("/.crosspoint/reading-stats/daily.1"));
  EXPECT_TRUE(backupPending(fs));
  fs.corruptWrite.clear();
  EXPECT_TRUE(restoreSnapshot(fs).ok());  // the retry rewrites it correctly
  EXPECT_EQ(fs.get("/.crosspoint/reading-stats/daily.1"), original);
}

TEST(NavRestoreSweep, ManyExtraStatisticsFilesAreAllRemovedInBatches) {
  FakeFs fs;
  seedCard(fs);
  const auto before = fs.files;
  ASSERT_TRUE(snapshot(fs).ok());
  for (int i = 0; i < 61; ++i) fs.put("/.crosspoint/reading-stats/run" + std::to_string(i) + ".0", Bytes{1});
  const auto result = restoreSnapshot(fs);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.extrasRemoved, 61u);
  EXPECT_EQ(fs.files, before);
}

TEST(NavRestoreSweep, AnExtraThatCannotBeRemovedStopsTheSweepAndKeepsTheBackup) {
  FakeFs fs;
  seedCard(fs);
  ASSERT_TRUE(snapshot(fs).ok());
  for (int i = 0; i < 20; ++i) fs.put("/.crosspoint/reading-stats/run" + std::to_string(i) + ".0", Bytes{1});
  fs.failRemove.insert("/.crosspoint/reading-stats/run3.0");
  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.firstFailedHash, pathHash("/.crosspoint/reading-stats/run3.0"));
  EXPECT_TRUE(backupPending(fs));
}

TEST(NavRestoreSweep, AFileWhoseNameCollidesWithARecordedPathIsStillAnExtra) {
  // Two statistics paths with the same FNV-1a hash: the index must compare the recorded path itself.
  std::map<uint32_t, std::string> seen;
  std::string a;
  std::string b;
  const std::string prefix = "/.crosspoint/reading-stats/";
  for (uint32_t i = 0; i < 2000000 && b.empty(); ++i) {
    const std::string name = "c" + std::to_string(i);
    const uint32_t hash = pathHash((prefix + name).c_str());
    const auto it = seen.find(hash);
    if (it != seen.end()) {
      a = it->second;
      b = name;
    } else {
      seen.emplace(hash, name);
    }
  }
  ASSERT_FALSE(b.empty()) << "no FNV-1a collision found";
  ASSERT_EQ(pathHash((prefix + a).c_str()), pathHash((prefix + b).c_str()));

  FakeFs fs;
  fs.put(prefix + a, Bytes{1, 2, 3});
  ASSERT_TRUE(snapshot(fs).ok());
  fs.put(prefix + a, Bytes{9});
  fs.put(prefix + b, Bytes{7});  // created by the run; same hash as the recorded file
  const auto result = restoreSnapshot(fs);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.extrasRemoved, 1u);
  EXPECT_EQ(fs.get(prefix + a), (Bytes{1, 2, 3}));
  EXPECT_FALSE(fs.has(prefix + b));
}

TEST(NavSnapshot, StaleBackupFilesOfAnEarlierCrashAreClearedInBatches) {
  FakeFs fs;
  seedCard(fs);
  for (int i = 0; i < 45; ++i) fs.put(std::string(R::BACKUP_DIR) + "/d" + std::to_string(100 + i) + ".bin", Bytes{1});
  fs.put(R::MANIFEST_TMP, "half a manifest");
  ASSERT_TRUE(snapshot(fs).ok());
  EXPECT_FALSE(fs.has(std::string(R::BACKUP_DIR) + "/d100.bin"));
  EXPECT_FALSE(fs.has(std::string(R::BACKUP_DIR) + "/d144.bin"));
  EXPECT_FALSE(fs.has(R::MANIFEST_TMP));
  EXPECT_TRUE(fs.has(R::MANIFEST));
}

namespace {

// A card that behaves like DeviceFs: writeFile goes through a temporary beside the file and replaceFile, over a
// rename that cannot overwrite.
class DeviceLikeFs : public FakeFs {
 public:
  bool writeFile(const char* path, const uint8_t* data, size_t n) override {
    const std::string temporary = std::string(path) + ".nbtmp";
    put(temporary, Bytes(data, data + n));
    if (!replaceFile(*this, temporary.c_str(), path)) {
      files.erase(temporary);
      return false;
    }
    return true;
  }
};

const char* const DEST = "/.crosspoint/reading-stats/x.1";
const char* const ASIDE = "/.crosspoint/reading-stats/x.1.nbold";
const char* const SRC = "/.crosspoint/reading-stats/x.1.nvtmp";

}  // namespace

TEST(NavReplaceFile, ReplacesAnExistingFileAndLeavesNoAsideCopy) {
  FakeFs fs;
  fs.put(DEST, "old");
  fs.put(SRC, "new");
  EXPECT_TRUE(replaceFile(fs, SRC, DEST));
  EXPECT_EQ(fs.get(DEST), bytes("new"));
  EXPECT_FALSE(fs.has(SRC));
  EXPECT_FALSE(fs.has(ASIDE));
}

TEST(NavReplaceFile, ThePlainRenameNeverOverwritesSoTheDestinationIsNeverDeletedFirst) {
  FakeFs fs;
  fs.put(DEST, "old");
  fs.put(SRC, "new");
  EXPECT_FALSE(fs.renameFile(SRC, DEST));  // what the device's rename does with an existing destination
  EXPECT_EQ(fs.get(DEST), bytes("old"));
  EXPECT_TRUE(fs.has(SRC));
}

TEST(NavReplaceFile, AFailedMoveAsideLeavesTheDestinationUntouched) {
  FakeFs fs;
  fs.put(DEST, "old");
  fs.put(SRC, "new");
  fs.failRenameNth = 1;
  EXPECT_FALSE(replaceFile(fs, SRC, DEST));
  EXPECT_EQ(fs.get(DEST), bytes("old"));
  EXPECT_EQ(fs.get(SRC), bytes("new"));
  EXPECT_FALSE(fs.has(ASIDE));
}

TEST(NavReplaceFile, AFailedSecondRenameRollsTheOriginalBack) {
  FakeFs fs;
  fs.put(DEST, "old");
  fs.put(SRC, "new");
  fs.failRenameNth = 2;
  EXPECT_FALSE(replaceFile(fs, SRC, DEST));
  EXPECT_EQ(fs.get(DEST), bytes("old"));
  EXPECT_EQ(fs.get(SRC), bytes("new"));
  EXPECT_FALSE(fs.has(ASIDE));
}

TEST(NavReplaceFile, WhenTheRollbackFailsTooTheOriginalIsStillOnTheCardAndTheNextAttemptRecoversIt) {
  FakeFs fs;
  fs.put(DEST, "old");
  fs.put(SRC, "new");
  fs.failRenameFrom = 2;  // the move-in and the rollback both fail
  EXPECT_FALSE(replaceFile(fs, SRC, DEST));
  EXPECT_FALSE(fs.has(DEST));
  EXPECT_EQ(fs.get(ASIDE), bytes("old"));  // nothing was deleted
  EXPECT_EQ(fs.get(SRC), bytes("new"));

  fs.failRenameFrom = 0;  // the card recovers: a destination with only an aside copy is a normal replace
  EXPECT_TRUE(replaceFile(fs, SRC, DEST));
  EXPECT_EQ(fs.get(DEST), bytes("new"));
  EXPECT_FALSE(fs.has(ASIDE));
}

TEST(NavReplaceFile, ALeftoverAsideCopyIsPutBackWhenTheNewFileCannotBeMovedIn) {
  FakeFs fs;  // a power loss between the two renames: the destination is gone, its content is the aside copy
  fs.put(ASIDE, "old");
  fs.put(SRC, "new");
  fs.failRenameNth = 1;
  EXPECT_FALSE(replaceFile(fs, SRC, DEST));
  EXPECT_EQ(fs.get(DEST), bytes("old"));
  EXPECT_FALSE(fs.has(ASIDE));
}

TEST(NavReplaceFile, ALeftoverAsideCopyBesideAnExistingDestinationIsReplacedNotKept) {
  FakeFs fs;
  fs.put(DEST, "current");
  fs.put(ASIDE, "older");
  fs.put(SRC, "new");
  EXPECT_TRUE(replaceFile(fs, SRC, DEST));
  EXPECT_EQ(fs.get(DEST), bytes("new"));
  EXPECT_FALSE(fs.has(ASIDE));
}

TEST(NavReplaceFile, ALeftoverAsideCopyThatCannotBeRemovedStopsBeforeTheDestinationIsTouched) {
  FakeFs fs;
  fs.put(DEST, "current");
  fs.put(ASIDE, "older");
  fs.put(SRC, "new");
  fs.failRemove.insert(ASIDE);
  EXPECT_FALSE(replaceFile(fs, SRC, DEST));
  EXPECT_EQ(fs.get(DEST), bytes("current"));
  EXPECT_EQ(fs.renames, 0);
}

TEST(NavReplaceFile, AFailingCleanupOfTheAsideCopyStillCountsAsReplaced) {
  FakeFs fs;
  fs.put(DEST, "old");
  fs.put(SRC, "new");
  fs.failRemove.insert(ASIDE);
  EXPECT_TRUE(replaceFile(fs, SRC, DEST));
  EXPECT_EQ(fs.get(DEST), bytes("new"));
}

TEST(NavRestoreDeviceFs, ARenameFailingAtAnyPointNeverLosesStateAndARetryFinishesTheRestore) {
  int failures = 0;
  int completed = 0;
  for (int k = 1; k <= 80; ++k) {
    DeviceLikeFs fs;
    seedCard(fs);
    fs.put("/.crosspoint/reading-stats/daily.1", patterned(9000, 1));  // restored through a temporary and a rename
    const auto before = fs.files;
    ASSERT_TRUE(snapshot(fs).ok());
    mutateCard(fs);
    fs.put("/.crosspoint/reading-stats/daily.1", patterned(9000, 2));
    const auto mutated = fs.files;

    fs.renames = 0;  // count only the restore's renames
    fs.failRenameNth = k;
    const auto first = restoreSnapshot(fs);
    fs.failRenameNth = 0;
    if (first.ok()) {
      ++completed;
      EXPECT_EQ(fs.files, before) << "k=" << k;
      continue;
    }
    ++failures;
    EXPECT_TRUE(backupPending(fs)) << "k=" << k;  // the backup is never discarded on a failure
    for (const auto& [path, data] : before) {
      if (!mutated.count(path)) continue;
      EXPECT_TRUE(fs.has(path)) << "k=" << k << " lost " << path;  // a single failed rename deletes nothing
    }
    for (const auto& [path, data] : fs.files) EXPECT_EQ(path.find(".nbold"), std::string::npos) << "k=" << k << path;

    const auto retry = restoreSnapshot(fs);
    EXPECT_TRUE(retry.ok()) << "k=" << k;
    EXPECT_EQ(fs.files, before) << "k=" << k;
  }
  EXPECT_GT(failures, 10);  // the sweep really hit renames inside the restore
  EXPECT_GT(completed, 0);  // and ran past the last one
}

TEST(NavRestoreDeviceFs, ARestoreThatLosesBothRenamesKeepsTheOriginalRecoverableAndTheBackup) {
  DeviceLikeFs fs;
  seedCard(fs);
  const auto before = fs.files;
  ASSERT_TRUE(snapshot(fs).ok());
  mutateCard(fs);
  fs.renames = 0;
  fs.failRenameFrom = 2;  // the first replace moves the old file aside and then nothing renames any more
  const auto result = restoreSnapshot(fs);
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(backupPending(fs));
  EXPECT_TRUE(backupDirHoldsFiles(fs));
  fs.failRenameFrom = 0;
  EXPECT_TRUE(restoreSnapshot(fs).ok());  // the next boot finishes the job from the retained backup
  for (const auto& [path, data] : before) {
    ASSERT_TRUE(fs.has(path)) << path;
    EXPECT_EQ(fs.get(path), data) << path;
  }
  for (const auto& [path, data] : fs.files) EXPECT_EQ(path.find(".nbold"), std::string::npos) << path;
}
