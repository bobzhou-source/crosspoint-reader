#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

// Backup and restore of the user's persisted reading state around a navigation run.
//
// A navigation script that opens books, turns pages and toggles night mode would
// otherwise leave the owner's recents, reading positions, statistics and settings
// changed. snapshot() copies exactly the state a run can touch into a manifest on the
// card; restore() puts it back and removes anything the run created next to it.
//
// The layer is pure: all storage goes through RestoreFs, so the restore paths are host
// tested with an in-memory card that can fail on demand. The manifest is written last,
// via a temporary file, so a half-written snapshot never looks complete; a complete
// manifest that is still present means a run died before restoring and must be restored
// before anything else runs.
//
// Memory is bounded and independent of the card: directories are walked as a stream (no list of names or paths),
// files are copied and checksummed in 1 KiB chunks, and the one table that scales (the restore's index of recorded
// paths, at most MAX_ENTRIES slots) is allocated without exceptions. A card that exceeds an explicit cap
// (MAX_ENTRIES files to protect, MAX_FILE_BYTES per file) is refused before the run starts; nothing is ever
// truncated or skipped.
namespace benchmark::nav {

class RestoreFs {
 public:
  virtual ~RestoreFs() = default;
  virtual bool exists(const char* path) = 0;
  // Size of a file; false when it is missing or cannot be read.
  virtual bool fileSize(const char* path, uint32_t& size) = 0;
  // Reads up to cap bytes at offset. n is the number read (0 at EOF). False on I/O error.
  virtual bool readAt(const char* path, uint32_t offset, uint8_t* buf, size_t cap, size_t& n) = 0;
  // Replaces a file: the bytes go to a temporary beside it, which then replaces the file through replaceFile();
  // creates missing parents. A failure leaves the previous content in place or recoverable (see replaceFile).
  virtual bool writeFile(const char* path, const uint8_t* data, size_t n) = 0;
  // Appends to a file, creating it first when absent.
  virtual bool appendFile(const char* path, const uint8_t* data, size_t n) = 0;
  virtual bool removeFile(const char* path) = 0;
  // Renames a file. Like FAT, it FAILS when `to` already exists and it never deletes anything. Replacing a file goes
  // through replaceFile().
  virtual bool renameFile(const char* from, const char* to) = 0;
  virtual bool makeDir(const char* path) = 0;
  // Visits each entry of a directory by bare name; the visitor returns false to stop.
  // False when the directory cannot be opened (an absent directory is not an error for
  // the callers; they check exists() first).
  virtual bool listDir(const char* dir, const std::function<bool(const char* name, bool isDir)>& visit) = 0;
};

namespace restore {
constexpr char BACKUP_DIR[] = "/.crosspoint/benchmark/nav-backup";
constexpr char MANIFEST[] = "/.crosspoint/benchmark/nav-backup/manifest.txt";
constexpr char MANIFEST_TMP[] = "/.crosspoint/benchmark/nav-backup/manifest.tmp";
constexpr char STATS_DIR[] = "/.crosspoint/reading-stats";
constexpr char CACHE_ROOT[] = "/.crosspoint";
constexpr char PROGRESS_NAME[] = "progress.bin";
constexpr uint32_t MAX_ENTRIES = 1536;
constexpr uint32_t MAX_FILE_BYTES = 32768;
constexpr uint32_t INLINE_BYTES = 48;
constexpr size_t MAX_LINE = 384;

// The files a run can change besides the per-book progress and statistics.
extern const char* const FIXED_FILES[];
extern const size_t FIXED_FILE_COUNT;
}  // namespace restore

enum class SnapshotError : uint8_t { None, Pending, Io, TooLarge, TooMany, OutOfMemory };

struct SnapshotResult {
  SnapshotError error = SnapshotError::None;
  uint32_t entries = 0;     // files recorded (present or absent)
  uint32_t present = 0;     // of those, files that existed and were copied
  uint32_t failedHash = 0;  // hash of the path that could not be recorded
  bool ok() const { return error == SnapshotError::None; }
};

struct RestoreResult {
  bool found = false;        // a complete manifest was present
  bool valid = true;         // manifest checksum and count matched
  uint32_t attempted = 0;    // entries processed
  uint32_t restored = 0;     // files rewritten and verified by read-back
  uint32_t removed = 0;      // files that did not exist at snapshot time and were deleted
  uint32_t extrasRemoved = 0;  // files the run created next to the snapshot and were deleted
  uint32_t failed = 0;       // entries or sweeps that could not be put back
  uint32_t firstFailedHash = 0;
  bool discarded = false;    // backup removed after a clean restore
  // Nothing to restore (no manifest) counts as clean; a damaged manifest does not.
  bool ok() const { return valid && failed == 0; }
};

// True when a complete manifest from an earlier run is on the card.
bool backupPending(RestoreFs& fs);

// Records the state a run can change. Refuses (Pending) when an unrestored snapshot exists.
SnapshotResult snapshot(RestoreFs& fs);

// Puts the snapshot back, verifies each file by reading it again, deletes files the run
// created beside it, and removes the backup only when everything succeeded. A failed
// restore keeps the backup so the next boot can try again.
RestoreResult restoreSnapshot(RestoreFs& fs);

// Moves `from` over `to` on a card whose rename cannot overwrite (FAT). The previous `to` is first renamed to
// `to` + ".nbold", never deleted, then `from` takes its place, then the old copy is removed. If the second step
// fails the old copy is renamed back. Failure contract:
//  - false before anything moved: `to` is untouched;
//  - false after the move aside: `to` is back, unless the rename back failed too, in which case the previous content
//    is still on the card as `to` + ".nbold" (and `from` is still there);
//  - a leftover `to` + ".nbold" from a power loss is either an older copy (when `to` exists, removed first) or the only
//    copy (when `to` is missing; it is renamed back if the new `from` cannot be moved in).
// A power loss between the two renames leaves `to` missing and its previous content at `to` + ".nbold".
bool replaceFile(RestoreFs& fs, const char* from, const char* to);

// FNV-1a of a path, as reported in logs.
uint32_t pathHash(const char* path);
uint32_t crc32(const uint8_t* data, size_t n, uint32_t seed = 0);

}  // namespace benchmark::nav
