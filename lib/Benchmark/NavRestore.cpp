#include "NavRestore.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace benchmark::nav {

namespace restore {
const char* const FIXED_FILES[] = {"/.crosspoint/recent.json", "/.crosspoint/state.json", "/.crosspoint/settings.json",
                                   "/.crosspoint/reader-sync/status.json"};
const size_t FIXED_FILE_COUNT = sizeof(FIXED_FILES) / sizeof(FIXED_FILES[0]);
}  // namespace restore

namespace {
using namespace restore;

// The device is built without exceptions and without PSRAM: nothing here grows with the card. Directory traversal
// streams (no name or path list), file copies and checksums go through one fixed work buffer, and the only table
// that scales (the restore's index of recorded paths) is sized from the validated entry count, which is capped by
// MAX_ENTRIES, and allocated with nothrow.
constexpr char MAGIC[] = "NAVBK2";
constexpr const char* CACHE_PREFIXES[] = {"epub_", "txt_", "xtc_"};
constexpr size_t CHUNK = 1024;            // copy / checksum / manifest read granule
constexpr size_t MANIFEST_BUFFER = 1024;  // manifest lines are appended in writes of this size
constexpr size_t PATH_CHARS = 300;        // longest path built from a directory entry (name <= 255)
constexpr size_t STATS_BATCH = 8;         // extra statistics files deleted per directory pass
constexpr size_t STATS_NAME = 64;
constexpr size_t CLEAN_BATCH = 16;        // backup files deleted per directory pass
constexpr size_t CLEAN_NAME = 32;

struct Work {
  uint8_t chunk[CHUNK];
  uint8_t manifest[MANIFEST_BUFFER];
};
std::unique_ptr<Work> makeWork() { return std::unique_ptr<Work>(new (std::nothrow) Work); }

uint32_t crcStep(uint32_t crc, const uint8_t* data, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

bool startsWith(const char* text, const char* prefix) { return strncmp(text, prefix, strlen(prefix)) == 0; }

bool isCacheDir(const char* name) {
  for (const char* prefix : CACHE_PREFIXES)
    if (startsWith(name, prefix)) return true;
  return false;
}

void dataPath(char* out, size_t cap, uint32_t idx) { snprintf(out, cap, "%s/d%u.bin", BACKUP_DIR, static_cast<unsigned>(idx)); }

void toHex(const uint8_t* data, size_t n, char* out) {
  static const char DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[2 * i] = DIGITS[data[i] >> 4];
    out[2 * i + 1] = DIGITS[data[i] & 15];
  }
  out[2 * n] = '\0';
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool fromHex(const char* text, uint8_t* out, size_t expected) {
  if (strlen(text) != 2 * expected) return false;
  for (size_t i = 0; i < expected; ++i) {
    const int hi = hexValue(text[2 * i]);
    const int lo = hexValue(text[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>(hi * 16 + lo);
  }
  return true;
}

// Reads exactly `want` bytes at `offset`. False on an I/O error or a file shorter than that.
bool readExact(RestoreFs& fs, const char* path, uint32_t offset, uint8_t* buf, size_t want) {
  size_t done = 0;
  while (done < want) {
    size_t n = 0;
    if (!fs.readAt(path, offset + static_cast<uint32_t>(done), buf + done, want - done, n) || n == 0) return false;
    done += n;
  }
  return true;
}

// True when the file has no byte at `offset`.
bool atEnd(RestoreFs& fs, const char* path, uint32_t offset, uint8_t* buf) {
  size_t n = 0;
  return fs.readAt(path, offset, buf, 1, n) && n == 0;
}

// The file has exactly `size` bytes and they checksum to `crc`; read in chunks, never whole.
bool fileMatches(RestoreFs& fs, const char* path, uint32_t size, uint32_t crc, uint8_t* buf) {
  uint32_t actual = 0;
  if (!fs.fileSize(path, actual) || actual != size) return false;
  uint32_t running = 0;
  for (uint32_t offset = 0; offset < size;) {
    const size_t want = std::min<size_t>(CHUNK, size - offset);
    if (!readExact(fs, path, offset, buf, want)) return false;
    running = crc32(buf, want, running);
    offset += static_cast<uint32_t>(want);
  }
  return running == crc;
}

// Copies `size` bytes (> 0) from `src` to `dst`: the first chunk replaces the destination, the rest is appended.
bool copyFile(RestoreFs& fs, const char* src, const char* dst, uint32_t size, uint8_t* buf) {
  for (uint32_t offset = 0; offset < size;) {
    const size_t want = std::min<size_t>(CHUNK, size - offset);
    if (!readExact(fs, src, offset, buf, want)) return false;
    if (!(offset == 0 ? fs.writeFile(dst, buf, want) : fs.appendFile(dst, buf, want))) return false;
    offset += static_cast<uint32_t>(want);
  }
  return true;
}

// Streams a text file line by line (no newline in the callback) in 256-byte reads. The callback receives the line, its
// length and the file offset of its first byte, and returns false to stop. False on I/O error, an over-long line or a
// stop.
template <typename Visit>
bool forEachLine(RestoreFs& fs, const char* path, Visit&& visit) {
  uint8_t chunk[256];  // its own buffer: the callback is free to use the work buffer while the scan is paused
  char line[MAX_LINE + 1];
  size_t used = 0;
  uint32_t offset = 0;
  uint32_t lineStart = 0;
  for (;;) {
    size_t n = 0;
    if (!fs.readAt(path, offset, chunk, sizeof(chunk), n)) return false;
    if (n == 0) break;
    for (size_t i = 0; i < n; ++i) {
      if (chunk[i] == '\n') {
        line[used] = '\0';
        if (!visit(line, used, lineStart)) return false;
        used = 0;
        lineStart = offset + static_cast<uint32_t>(i) + 1;
      } else {
        if (used >= MAX_LINE) return false;
        line[used++] = static_cast<char>(chunk[i]);
      }
    }
    offset += static_cast<uint32_t>(n);
  }
  return used == 0;  // the manifest always ends with a newline
}

// Deletes every file in the backup directory, a few names per directory pass (nothing is deleted while the directory
// is being walked, and no name list grows with the card). Foreign names too long to hold are left alone.
bool cleanBackupDir(RestoreFs& fs, uint32_t& failedHash) {
  if (!fs.exists(BACKUP_DIR)) return true;
  struct Batch {
    char names[CLEAN_BATCH][CLEAN_NAME];
    size_t count = 0;
  };
  for (;;) {
    Batch batch;
    Batch* const target = &batch;
    const bool listed = fs.listDir(BACKUP_DIR, [target](const char* name, bool isDir) {
      if (!isDir && strlen(name) < CLEAN_NAME) {
        strcpy(target->names[target->count], name);
        ++target->count;
      }
      return target->count < CLEAN_BATCH;
    });
    if (!listed) return false;
    if (batch.count == 0) return true;
    for (size_t i = 0; i < batch.count; ++i) {
      char path[PATH_CHARS];
      snprintf(path, sizeof(path), "%s/%s", BACKUP_DIR, batch.names[i]);
      if (!fs.removeFile(path)) {
        failedHash = pathHash(path);
        return false;
      }
    }
  }
}

// Every path a run can touch, streamed in a stable order: statistics files, then the progress file of each book cache
// that has one. `visit(path, present)` returns false to stop; `present` is true when the walk already saw the file.
enum class Walk : uint8_t { Done, Stopped, Io };

template <typename Visit>
struct WalkState {
  Visit* visit;
  bool stopped = false;
  bool io = false;
};

template <typename Visit>
Walk walkStats(RestoreFs& fs, Visit& visit) {
  if (!fs.exists(STATS_DIR)) return Walk::Done;
  WalkState<Visit> state{&visit};
  WalkState<Visit>* const s = &state;
  const bool ok = fs.listDir(STATS_DIR, [s](const char* name, bool isDir) {
    if (isDir) return true;
    char path[PATH_CHARS];
    const int n = snprintf(path, sizeof(path), "%s/%s", STATS_DIR, name);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) {
      s->io = true;
      return false;
    }
    if (!(*s->visit)(path, true)) {
      s->stopped = true;
      return false;
    }
    return true;
  });
  if (!ok || state.io) return Walk::Io;
  return state.stopped ? Walk::Stopped : Walk::Done;
}

template <typename Visit>
Walk walkCaches(RestoreFs& fs, Visit& visit) {
  WalkState<Visit> state{&visit};
  WalkState<Visit>* const s = &state;
  RestoreFs* const card = &fs;
  if (!fs.exists(CACHE_ROOT)) return Walk::Done;
  const bool ok = fs.listDir(CACHE_ROOT, [s, card](const char* name, bool isDir) {
    if (!isDir || !isCacheDir(name)) return true;
    char path[PATH_CHARS];
    const int n = snprintf(path, sizeof(path), "%s/%s/%s", CACHE_ROOT, name, PROGRESS_NAME);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) {
      s->io = true;
      return false;
    }
    if (!card->exists(path)) return true;
    if (!(*s->visit)(path, true)) {
      s->stopped = true;
      return false;
    }
    return true;
  });
  if (!ok || state.io) return Walk::Io;
  return state.stopped ? Walk::Stopped : Walk::Done;
}

// Appends manifest text through the work buffer in large writes while keeping the body checksum.
class ManifestWriter {
 public:
  ManifestWriter(RestoreFs& fs, const char* path, uint8_t* buffer) : fs(fs), path(path), buffer(buffer) {}
  bool line(const char* text) {
    const size_t n = strlen(text);
    if (n + 1 > MANIFEST_BUFFER) return false;
    if (used + n + 1 > MANIFEST_BUFFER && !flush()) return false;
    memcpy(buffer + used, text, n);
    buffer[used + n] = '\n';
    used += n + 1;
    crc = crcStep(crc, reinterpret_cast<const uint8_t*>(text), n);
    const uint8_t nl = '\n';
    crc = crcStep(crc, &nl, 1);
    return true;
  }
  bool flush() {
    if (used == 0) return true;
    const bool ok = fs.appendFile(path, buffer, used);
    used = 0;
    return ok;
  }
  uint32_t checksum() const { return ~crc; }

 private:
  RestoreFs& fs;
  const char* path;
  uint8_t* buffer;
  size_t used = 0;
  uint32_t crc = 0xFFFFFFFFu;
};

struct Entry {
  uint32_t idx = 0;
  bool existed = false;
  uint32_t size = 0;
  uint32_t crc = 0;
  char inlineHex[2 * INLINE_BYTES + 1] = {};
  char path[MAX_LINE] = {};
};

bool parseEntry(const char* line, Entry& e) {
  // F \t idx \t existed \t size \t crc \t inline \t path
  char copy[MAX_LINE + 1];
  strncpy(copy, line, sizeof(copy) - 1);
  copy[sizeof(copy) - 1] = '\0';
  char* fields[7];
  size_t count = 0;
  char* cursor = copy;
  while (count < 7) {
    fields[count++] = cursor;
    if (count == 7) break;
    char* tab = strchr(cursor, '\t');
    if (!tab) return false;
    *tab = '\0';
    cursor = tab + 1;
  }
  if (strcmp(fields[0], "F") != 0) return false;
  char* end = nullptr;
  e.idx = static_cast<uint32_t>(strtoul(fields[1], &end, 10));
  if (*end || !*fields[1]) return false;
  if (strcmp(fields[2], "0") != 0 && strcmp(fields[2], "1") != 0) return false;
  e.existed = fields[2][0] == '1';
  e.size = static_cast<uint32_t>(strtoul(fields[3], &end, 10));
  if (*end || !*fields[3] || e.size > MAX_FILE_BYTES) return false;
  e.crc = static_cast<uint32_t>(strtoul(fields[4], &end, 16));
  if (*end || !*fields[4]) return false;
  if (strlen(fields[5]) > 2 * INLINE_BYTES) return false;
  strcpy(e.inlineHex, strcmp(fields[5], "-") == 0 ? "" : fields[5]);
  if (!*fields[6] || strlen(fields[6]) >= sizeof(e.path)) return false;
  strcpy(e.path, fields[6]);
  return true;
}

// ---- snapshot --------------------------------------------------------------------------------------------------

// Records each path the walk hands over: one manifest line, and a copy in the backup directory for files over the
// inline limit. Every cap (entry count, file size) fails here, before the run, and never truncates.
struct Recorder {
  RestoreFs& fs;
  ManifestWriter& manifest;
  uint8_t* buf;
  SnapshotResult& result;
  uint32_t idx = 0;

  bool fail(SnapshotError error, const char* path) {
    result.error = error;
    result.failedHash = pathHash(path);
    return false;
  }

  bool operator()(const char* path, bool present) {
    if (idx >= MAX_ENTRIES) return fail(SnapshotError::TooMany, path);
    const bool existed = present || fs.exists(path);
    uint32_t crc = 0;
    uint32_t size = 0;
    char inlineHex[2 * INLINE_BYTES + 1] = "-";
    if (existed) {
      if (!fs.fileSize(path, size)) return fail(SnapshotError::Io, path);
      if (size > MAX_FILE_BYTES) return fail(SnapshotError::TooLarge, path);
      char copy[96];
      dataPath(copy, sizeof(copy), idx);
      for (uint32_t offset = 0; offset < size;) {
        const size_t want = std::min<size_t>(CHUNK, size - offset);
        if (!readExact(fs, path, offset, buf, want)) return fail(SnapshotError::Io, path);
        crc = crc32(buf, want, crc);
        if (size <= INLINE_BYTES) {
          toHex(buf, size, inlineHex);
        } else if (!(offset == 0 ? fs.writeFile(copy, buf, want) : fs.appendFile(copy, buf, want))) {
          return fail(SnapshotError::Io, path);
        }
        offset += static_cast<uint32_t>(want);
      }
      // A file that grew while it was copied is not a faithful copy.
      if (!atEnd(fs, path, size, buf)) return fail(SnapshotError::Io, path);
      ++result.present;
    }
    char line[MAX_LINE + 1];
    const int n = snprintf(line, sizeof(line), "F\t%u\t%d\t%u\t%08x\t%s\t%s", static_cast<unsigned>(idx), existed ? 1 : 0,
                           static_cast<unsigned>(size), static_cast<unsigned>(crc), size && size <= INLINE_BYTES ? inlineHex : "-",
                           path);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line) || !manifest.line(line)) return fail(SnapshotError::Io, path);
    ++idx;
    return true;
  }
};

// ---- restore ---------------------------------------------------------------------------------------------------

struct Known {
  uint32_t hash;
  uint32_t offset;  // start of the manifest line that recorded the path
};

enum class Lookup : uint8_t { Unknown, Known, Error };

// The recorded paths, found by hash and confirmed against the manifest line itself, so a hash collision can neither
// hide an extra file nor condemn a recorded one.
struct Index {
  RestoreFs& fs;
  uint8_t* buf;
  Known* known;
  uint32_t count;

  Lookup find(const char* path) const {
    const uint32_t hash = pathHash(path);
    const Known* it = std::lower_bound(known, known + count, hash, [](const Known& k, uint32_t h) { return k.hash < h; });
    for (; it != known + count && it->hash == hash; ++it) {
      char line[MAX_LINE + 1];
      size_t used = 0;
      bool whole = false;
      for (uint32_t offset = it->offset; !whole;) {
        size_t n = 0;
        if (!fs.readAt(MANIFEST, offset, buf, CHUNK, n) || n == 0) return Lookup::Error;
        for (size_t i = 0; i < n && !whole; ++i) {
          if (buf[i] == '\n') {
            whole = true;
          } else {
            if (used >= MAX_LINE) return Lookup::Error;
            line[used++] = static_cast<char>(buf[i]);
          }
        }
        offset += static_cast<uint32_t>(n);
      }
      line[used] = '\0';
      Entry e;
      if (!parseEntry(line, e)) return Lookup::Error;
      if (strcmp(e.path, path) == 0) return Lookup::Known;
    }
    return Lookup::Unknown;
  }
};

struct Restorer {
  RestoreFs& fs;
  RestoreResult& result;
  uint8_t* buf;
  Index index;

  void fail(const char* path) {
    if (!result.failed) result.firstFailedHash = pathHash(path);
    ++result.failed;
  }

  // Puts one recorded file back; every file is checked by reading it again.
  void restoreEntry(const Entry& e) {
    ++result.attempted;
    if (!e.existed) {
      if (fs.exists(e.path)) {
        if (fs.removeFile(e.path)) ++result.removed;
        else fail(e.path);
      }
      return;
    }
    bool ok = false;
    if (e.size <= INLINE_BYTES) {
      uint8_t inlined[INLINE_BYTES];
      ok = fromHex(e.inlineHex, inlined, e.size) && crc32(inlined, e.size) == e.crc &&
           fs.writeFile(e.path, inlined, e.size);
    } else {
      char source[96];
      dataPath(source, sizeof(source), e.idx);
      // The backup copy must be intact before the destination is touched.
      if (fileMatches(fs, source, e.size, e.crc, buf)) {
        if (e.size <= CHUNK) {
          ok = copyFile(fs, source, e.path, e.size, buf);
        } else {
          // Several writes: assemble beside the destination, then rename over it.
          char temporary[MAX_LINE + 8];
          snprintf(temporary, sizeof(temporary), "%s.nvtmp", e.path);
          ok = copyFile(fs, source, temporary, e.size, buf) && replaceFile(fs, temporary, e.path);
          if (!ok) fs.removeFile(temporary);
        }
      }
    }
    if (ok && fileMatches(fs, e.path, e.size, e.crc, buf)) ++result.restored;
    else fail(e.path);
  }

  // A file next to the snapshot that the snapshot did not record: the run made it.
  struct SweepCache {
    Restorer& self;
    bool operator()(const char* path, bool) {
      switch (self.index.find(path)) {
        case Lookup::Known: break;
        case Lookup::Error: self.fail(path); break;
        case Lookup::Unknown:
          if (self.fs.removeFile(path)) ++self.result.extrasRemoved;
          else self.fail(path);
          break;
      }
      return true;
    }
  };

  // Statistics files are removed a few at a time between directory passes, never while the directory is walked.
  struct SweepStats {
    Restorer& self;
    char names[STATS_BATCH][STATS_NAME];
    size_t count = 0;
    uint32_t problems = 0;  // names that cannot be judged or held; they stay put, so only the final pass reports them
    uint32_t firstProblem = 0;

    bool operator()(const char* name, bool isDir) {
      if (isDir) return true;
      char path[PATH_CHARS];
      snprintf(path, sizeof(path), "%s/%s", STATS_DIR, name);
      const Lookup found = self.index.find(path);
      if (found == Lookup::Known) return true;
      if (found == Lookup::Error || strlen(name) >= STATS_NAME) {
        if (!problems++) firstProblem = pathHash(path);
        return true;
      }
      strcpy(names[count++], name);
      return count < STATS_BATCH;
    }
  };

  void sweepStats() {
    if (!fs.exists(STATS_DIR)) return;
    for (;;) {
      SweepStats pass{*this, {}, 0, 0, 0};
      SweepStats* const target = &pass;
      if (!fs.listDir(STATS_DIR, [target](const char* name, bool isDir) { return (*target)(name, isDir); })) {
        fail(STATS_DIR);
        return;
      }
      if (pass.count == 0) {
        if (pass.problems) {
          if (!result.failed) result.firstFailedHash = pass.firstProblem;
          result.failed += pass.problems;
        }
        return;
      }
      for (size_t i = 0; i < pass.count; ++i) {
        char path[PATH_CHARS];
        snprintf(path, sizeof(path), "%s/%s", STATS_DIR, pass.names[i]);
        if (fs.removeFile(path)) {
          ++result.extrasRemoved;
        } else {
          fail(path);
          return;
        }
      }
    }
  }

  void sweepCaches() {
    SweepCache sweep{*this};
    if (walkCaches(fs, sweep) != Walk::Done) fail(CACHE_ROOT);
  }
};

}  // namespace

uint32_t pathHash(const char* path) {
  uint32_t h = 2166136261u;
  for (; *path; ++path) h = (h ^ static_cast<uint8_t>(*path)) * 16777619u;
  return h;
}

uint32_t crc32(const uint8_t* data, size_t n, uint32_t seed) { return ~crcStep(~seed, data, n); }

bool backupPending(RestoreFs& fs) { return fs.exists(MANIFEST); }

bool replaceFile(RestoreFs& fs, const char* from, const char* to) {
  char aside[MAX_LINE + 16];
  const int n = snprintf(aside, sizeof(aside), "%s.nbold", to);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(aside)) return false;
  if (fs.exists(to)) {
    // A leftover aside copy is older than the file now at `to`.
    if (fs.exists(aside) && !fs.removeFile(aside)) return false;
    if (!fs.renameFile(to, aside)) return false;
  }
  if (!fs.renameFile(from, to)) {
    // Put the previous content back; if that fails too it stays recoverable at `aside`.
    if (!fs.exists(to) && fs.exists(aside)) fs.renameFile(aside, to);
    return false;
  }
  if (fs.exists(aside)) fs.removeFile(aside);  // best effort: the new content is already in place
  return true;
}

SnapshotResult snapshot(RestoreFs& fs) {
  SnapshotResult result;
  if (backupPending(fs)) {
    result.error = SnapshotError::Pending;
    return result;
  }
  if (!fs.makeDir(BACKUP_DIR)) {
    result.error = SnapshotError::Io;
    return result;
  }
  const auto work = makeWork();
  if (!work) {
    result.error = SnapshotError::OutOfMemory;
    return result;
  }
  // A crash before the manifest was renamed leaves data files that nothing refers to.
  if (!cleanBackupDir(fs, result.failedHash)) {
    result.error = SnapshotError::Io;
    return result;
  }

  ManifestWriter manifest(fs, MANIFEST_TMP, work->manifest);
  Recorder recorder{fs, manifest, work->chunk, result};
  bool complete = manifest.line(MAGIC);
  if (!complete) result.error = SnapshotError::Io;
  for (size_t i = 0; complete && i < FIXED_FILE_COUNT; ++i) complete = recorder(FIXED_FILES[i], false);
  if (complete) {
    const Walk stats = walkStats(fs, recorder);
    complete = stats == Walk::Done;
    if (stats == Walk::Io) result.error = SnapshotError::Io;
  }
  if (complete) {
    const Walk caches = walkCaches(fs, recorder);
    complete = caches == Walk::Done;
    if (caches == Walk::Io) result.error = SnapshotError::Io;
  }
  if (complete) {
    // The END line is outside the body checksum it records.
    char end[48];
    snprintf(end, sizeof(end), "END\t%08x\t%u", static_cast<unsigned>(manifest.checksum()),
             static_cast<unsigned>(recorder.idx));
    const size_t endLen = strlen(end);
    end[endLen] = '\n';
    complete = manifest.flush() &&
               fs.appendFile(MANIFEST_TMP, reinterpret_cast<const uint8_t*>(end), endLen + 1) &&
               fs.renameFile(MANIFEST_TMP, MANIFEST);
    if (!complete) result.error = SnapshotError::Io;
  }
  if (!complete) {
    // No manifest was published: nothing is half protected. Drop what was copied so far.
    uint32_t ignored = 0;
    cleanBackupDir(fs, ignored);
    return result;
  }
  result.entries = recorder.idx;
  return result;
}

RestoreResult restoreSnapshot(RestoreFs& fs) {
  RestoreResult result;
  if (!fs.exists(MANIFEST)) return result;
  result.found = true;

  const auto work = makeWork();
  if (!work) {
    // Nothing was changed and the backup stays; the next boot retries with a freer heap.
    result.failed = 1;
    result.firstFailedHash = pathHash(MANIFEST);
    return result;
  }

  // Pass 1: the manifest must be whole before anything is changed.
  uint32_t entries = 0;
  uint32_t declared = 0;
  uint32_t crc = 0xFFFFFFFFu;
  bool sawHead = false;
  bool sawEnd = false;
  const bool read = forEachLine(fs, MANIFEST, [&](const char* line, size_t n, uint32_t) {
    if (sawEnd) return false;
    if (!sawHead) {
      if (strcmp(line, MAGIC) != 0) return false;
      sawHead = true;
    } else if (strncmp(line, "END\t", 4) == 0) {
      char* end = nullptr;
      const uint32_t stored = static_cast<uint32_t>(strtoul(line + 4, &end, 16));
      if (*end != '\t') return false;
      declared = static_cast<uint32_t>(strtoul(end + 1, &end, 10));
      if (*end || declared != entries || stored != ~crc) return false;
      sawEnd = true;
      return true;
    } else {
      Entry e;
      if (!parseEntry(line, e) || e.idx != entries || entries >= MAX_ENTRIES) return false;
      ++entries;
    }
    const uint8_t nl = '\n';
    crc = crcStep(crc, reinterpret_cast<const uint8_t*>(line), n);
    crc = crcStep(crc, &nl, 1);
    return true;
  });
  if (!read || !sawHead || !sawEnd) {
    result.valid = false;
    result.failed = 1;
    result.firstFailedHash = pathHash(MANIFEST);
    return result;
  }

  std::unique_ptr<Known[]> known;
  if (entries) {
    known.reset(new (std::nothrow) Known[entries]);
    if (!known) {
      result.failed = 1;
      result.firstFailedHash = pathHash(MANIFEST);
      return result;
    }
  }

  // Pass 2: put every recorded file back, indexing each recorded path by hash and manifest offset.
  Restorer restorer{fs, result, work->chunk, Index{fs, work->chunk, known.get(), entries}};
  uint32_t indexed = 0;
  forEachLine(fs, MANIFEST, [&](const char* line, size_t, uint32_t offset) {
    if (strncmp(line, "F\t", 2) != 0) return true;
    Entry e;
    if (!parseEntry(line, e) || indexed >= entries) return true;  // pass 1 validated every line
    known[indexed++] = Known{pathHash(e.path), offset};
    restorer.restoreEntry(e);
    return true;
  });
  std::sort(known.get(), known.get() + indexed, [](const Known& a, const Known& b) { return a.hash < b.hash; });

  // Pass 3: delete what the run created next to the snapshot (statistics of books it opened, progress of books that
  // had none).
  restorer.sweepStats();
  restorer.sweepCaches();

  if (result.failed) return result;  // keep the backup so the next boot can try again

  // The manifest goes first: once it is gone the restore is complete; a data file that cannot be removed is cleared
  // by the next snapshot.
  if (!fs.removeFile(MANIFEST)) {
    restorer.fail(MANIFEST);
    return result;
  }
  uint32_t ignored = 0;
  cleanBackupDir(fs, ignored);
  result.discarded = true;
  return result;
}

}  // namespace benchmark::nav
