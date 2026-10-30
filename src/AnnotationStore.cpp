#include "AnnotationStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PersistableStore.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
constexpr char ROOT[] = "/.crosspoint/annotations";
constexpr size_t MAX_FILE_BYTES = 8192;
constexpr uint8_t SCHEMA_VERSION = 1;
using Result = AnnotationStore::Result;

bool validPathText(const std::string& text) {
  if (text.empty() || text.size() > AnnotationEntry::MAX_PATH_BYTES) return false;
  for (const unsigned char c : text) {
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

std::string directoryPath(const std::string& bookPath) { return std::string(ROOT) + "/" + AnnotationStore::sourceKey(bookPath); }

bool validSourceKey(const std::string_view key) {
  if (key.size() != 16) return false;
  for (const char c : key) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

void recordPath(char* path, const size_t size, const char* directory, const uint16_t id) {
  snprintf(path, size, "%s/%03u.json", directory, static_cast<unsigned>(id));
}

// A failed replacement keeps the old file as .bak. Recover it before opening or
// modifying a record; an interrupted write never becomes an empty annotation.
bool recoverRecord(const char* path) {
  if (Storage.exists(path)) return true;
  char backup[96];
  snprintf(backup, sizeof(backup), "%s.bak", path);
  if (!Storage.exists(backup)) return true;
  if (Storage.rename(backup, path)) return true;
  LOG_ERR("ANN", "Failed to recover %s", path);
  return false;
}

bool readText(const JsonVariantConst value, std::string& text, const size_t limit) {
  const char* raw = value.as<const char*>();
  if (!raw || strnlen(raw, limit + 1) > limit) return false;
  text = raw;
  return true;
}

Result readEntry(const char* directory, const std::string_view bookPath, const uint16_t id, AnnotationEntry& entry) {
  char path[80];
  recordPath(path, sizeof(path), directory, id);
  if (!recoverRecord(path)) return Result::IoError;
  if (!Storage.exists(path)) return Result::NotFound;
  {
    HalFile file;
    if (!Storage.openFileForRead("ANN", path, file)) return Result::IoError;
    if (file.size() == 0 || file.size() > MAX_FILE_BYTES) {
      LOG_ERR("ANN", "Invalid annotation size: %s", path);
      return Result::InvalidData;
    }
  }

  // Share the project's JSON parser instantiation. The preflight above bounds
  // both its input string and document; only one record is parsed at a time.
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path, doc)) return Result::IoError;
  if (doc["version"] != SCHEMA_VERSION || doc["id"] != id ||
      (!doc["kind"].isNull() && (!doc["kind"].is<uint8_t>() ||
                               doc["kind"].as<uint8_t>() > static_cast<uint8_t>(AnnotationKind::ReviewLater))) ||
      !readText(doc["sourcePath"], entry.sourcePath, AnnotationEntry::MAX_PATH_BYTES) ||
      (!bookPath.empty() && entry.sourcePath != bookPath) ||
      !readText(doc["sourceTitle"], entry.sourceTitle, AnnotationEntry::MAX_TITLE_BYTES) ||
      !readText(doc["chapterTitle"], entry.chapterTitle, AnnotationEntry::MAX_TITLE_BYTES) ||
      !readText(doc["chapterHref"], entry.chapterHref, AnnotationEntry::MAX_PATH_BYTES) ||
      !readText(doc["xpath"], entry.location.xpath, AnnotationEntry::MAX_PATH_BYTES) ||
      !readText(doc["excerpt"], entry.excerpt, AnnotationEntry::MAX_EXCERPT_BYTES) ||
      !readText(doc["note"], entry.note, AnnotationEntry::MAX_NOTE_BYTES) ||
      !doc["si"].is<uint16_t>() || !doc["pc"].is<uint16_t>() || !doc["pp"].is<uint16_t>() ||
      !doc["percentage"].is<float>() || !doc["pageContext"].is<bool>() || !doc["createdAt"].is<uint64_t>() ||
      (!doc["vo"].isNull() && !doc["vo"].is<uint32_t>())) {
    LOG_ERR("ANN", "Invalid annotation record: %s", path);
    return Result::InvalidData;
  }
  entry.id = id;
  entry.location.percentage = doc["percentage"].as<float>();
  if (!std::isfinite(entry.location.percentage) || entry.location.percentage < 0 || entry.location.percentage > 1 ||
      !validPathText(entry.sourcePath) || !validPathText(entry.chapterHref) || !validPathText(entry.location.xpath)) {
    LOG_ERR("ANN", "Invalid annotation location: %s", path);
    return Result::InvalidData;
  }
  entry.location.computedSpineIndex = doc["si"].as<uint16_t>();
  entry.location.computedChapterPageCount = doc["pc"].as<uint16_t>();
  entry.location.computedChapterProgress = doc["pp"].as<uint16_t>();
  entry.location.hasVisibleTextOffset = !doc["vo"].isNull();
  entry.location.visibleTextOffset = doc["vo"] | static_cast<uint32_t>(0);
  entry.kind = static_cast<AnnotationKind>(doc["kind"].as<uint8_t>());
  entry.location.name.clear();
  entry.location.summary.clear();
  entry.pageContext = doc["pageContext"].as<bool>();
  entry.createdAt = doc["createdAt"].as<uint64_t>();
  return Result::Success;
}

bool writeEntry(const char* path, const AnnotationEntry& entry, const uint16_t id) {
  JsonDocument doc;
  doc["kind"] = static_cast<uint8_t>(entry.kind);
  doc["version"] = SCHEMA_VERSION;
  doc["id"] = id;
  doc["sourcePath"] = entry.sourcePath;
  doc["sourceTitle"] = entry.sourceTitle;
  doc["chapterTitle"] = entry.chapterTitle;
  doc["chapterHref"] = entry.chapterHref;
  doc["xpath"] = entry.location.xpath;
  doc["percentage"] = entry.location.percentage;
  doc["si"] = entry.location.computedSpineIndex;
  doc["pc"] = entry.location.computedChapterPageCount;
  doc["pp"] = entry.location.computedChapterProgress;
  if (entry.location.hasVisibleTextOffset) doc["vo"] = entry.location.visibleTextOffset;
  doc["excerpt"] = entry.excerpt;
  doc["note"] = entry.note;
  doc["pageContext"] = entry.pageContext;
  doc["createdAt"] = entry.createdAt;
  if (doc.overflowed() || measureJson(doc) > MAX_FILE_BYTES) {
    LOG_ERR("ANN", "Annotation JSON exceeded memory or size budget");
    return false;
  }

  char temporary[96];
  snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  bool written = false;
  {
    HalFile file;
    if (!Storage.openFileForWrite("ANN", temporary, file)) return false;
    written = serializeJson(doc, file) == measureJson(doc);
    file.flush();
    written = file.close() && written;  // Must close before rename/remove.
  }
  if (!written) {
    Storage.remove(temporary);
    LOG_ERR("ANN", "Failed to write %s", temporary);
    return false;
  }

  char backup[96];
  snprintf(backup, sizeof(backup), "%s.bak", path);
  const bool replacing = Storage.exists(path);
  if (replacing && ((Storage.exists(backup) && !Storage.remove(backup)) || !Storage.rename(path, backup))) {
    Storage.remove(temporary);
    LOG_ERR("ANN", "Failed to preserve %s", path);
    return false;
  }
  if (!Storage.rename(temporary, path)) {
    if (replacing) Storage.rename(backup, path);
    Storage.remove(temporary);
    LOG_ERR("ANN", "Failed to commit %s", path);
    return false;
  }
  if (replacing && !Storage.remove(backup)) LOG_ERR("ANN", "Could not remove old backup %s", backup);
  return true;
}
}  // namespace

std::string AnnotationStore::sourceKey(const std::string_view bookPath) {
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char c : bookPath) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  char key[17];
  snprintf(key, sizeof(key), "%08lx%08lx", static_cast<unsigned long>(hash >> 32),
           static_cast<unsigned long>(hash & 0xffffffffu));
  return key;
}

AnnotationStore::Result AnnotationStore::load(const std::string& bookPath, const uint16_t id, AnnotationEntry& entry) {
  if (!validPathText(bookPath) || id == 0 || id > MAX_ENTRIES) {
    LOG_ERR("ANN", "Invalid annotation identity");
    return Result::InvalidData;
  }
  return readEntry(directoryPath(bookPath).c_str(), bookPath, id, entry);
}

AnnotationStore::Result AnnotationStore::loadFromSourceKey(const std::string_view key, const uint16_t id,
                                                          AnnotationEntry& entry) {
  if (!validSourceKey(key) || id == 0 || id > MAX_ENTRIES) {
    LOG_ERR("ANN", "Invalid annotation source key");
    return Result::InvalidData;
  }
  char directory[48];
  snprintf(directory, sizeof(directory), "%s/%.*s", ROOT, static_cast<int>(key.size()), key.data());
  const Result loaded = readEntry(directory, {}, id, entry);
  if (loaded == Result::Success && sourceKey(entry.sourcePath) != key) {
    LOG_ERR("ANN", "Annotation source does not match its directory");
    return Result::InvalidData;
  }
  return loaded;
}

AnnotationStore::Result AnnotationStore::visitAll(const Visitor visitor, void* context) {
  if (!visitor) {
    LOG_ERR("ANN", "Missing annotation visitor");
    return Result::InvalidData;
  }
  if (!Storage.ready()) {
    LOG_ERR("ANN", "Annotation storage unavailable");
    return Result::IoError;
  }
  if (!Storage.exists(ROOT)) return Result::Success;
  auto root = Storage.open(ROOT);
  if (!root || !root.isDirectory()) {
    LOG_ERR("ANN", "Could not open annotation root");
    return Result::IoError;
  }
  // One bounded record is reused across every book; no full-library allocation.
  auto entry = makeUniqueNoThrow<AnnotationEntry>();
  if (!entry) {
    LOG_ERR("ANN", "OOM: cross-book annotation scan");
    return Result::IoError;
  }
  while (auto directory = root.openNextFile()) {
    char key[18] = {};
    if (directory.getName(key, sizeof(key)) == 0) {
      LOG_ERR("ANN", "Could not read annotation directory entry");
      return Result::IoError;
    }
    if (!validSourceKey(key)) continue;
    if (!directory.isDirectory()) {
      LOG_ERR("ANN", "Annotation source key is not a directory: %s", key);
      return Result::IoError;
    }
    // Scan native IDs once, including .bak recovery without duplicate entries.
    for (uint16_t id = 1; id <= MAX_ENTRIES; ++id) {
      if ((id % 16) == 0) vTaskDelay(1);
      const Result loaded = loadFromSourceKey(key, id, *entry);
      if (loaded == Result::NotFound) continue;
      if (loaded != Result::Success) return loaded;
      const Result visited = visitor(*entry, context);
      if (visited != Result::Success) return visited;
    }
  }
  return Result::Success;
}

AnnotationStore::Result AnnotationStore::save(AnnotationEntry& entry) {
  if (entry.id > MAX_ENTRIES || static_cast<uint8_t>(entry.kind) > static_cast<uint8_t>(AnnotationKind::ReviewLater) ||
      !validPathText(entry.sourcePath) || !validPathText(entry.chapterHref) ||
      !validPathText(entry.location.xpath) || entry.sourceTitle.size() > AnnotationEntry::MAX_TITLE_BYTES ||
      entry.chapterTitle.size() > AnnotationEntry::MAX_TITLE_BYTES || entry.excerpt.empty() ||
      entry.excerpt.size() > AnnotationEntry::MAX_EXCERPT_BYTES || entry.note.size() > AnnotationEntry::MAX_NOTE_BYTES ||
      !std::isfinite(entry.location.percentage) || entry.location.percentage < 0 || entry.location.percentage > 1) {
    LOG_ERR("ANN", "Invalid annotation data");
    return Result::InvalidData;
  }
  const std::string directory = directoryPath(entry.sourcePath);
  if (!Storage.mkdir(directory.c_str())) {
    LOG_ERR("ANN", "Could not create annotation directory");
    return Result::IoError;
  }
  uint16_t id = entry.id;
  char path[80];
  if (id == 0) {
    for (id = 1; id <= MAX_ENTRIES; ++id) {
      recordPath(path, sizeof(path), directory.c_str(), id);
      if (!recoverRecord(path)) return Result::IoError;
      if (!Storage.exists(path)) break;
    }
    if (id > MAX_ENTRIES) {
      LOG_ERR("ANN", "Annotation limit reached");
      return Result::LimitReached;
    }
  } else {
    // Validate identity before replacing anything, including the unlikely case of
    // a source-key collision. One temporary entry is bounded to under 4 KiB.
    auto previous = makeUniqueNoThrow<AnnotationEntry>();
    if (!previous) {
      LOG_ERR("ANN", "OOM: annotation update");
      return Result::IoError;
    }
    const Result loaded = readEntry(directory.c_str(), entry.sourcePath, id, *previous);
    if (loaded != Result::Success) return loaded;
    recordPath(path, sizeof(path), directory.c_str(), id);
  }
  if (!writeEntry(path, entry, id)) return Result::IoError;
  entry.id = id;
  return Result::Success;
}

AnnotationStore::Result AnnotationStore::remove(const std::string& bookPath, const uint16_t id) {
  auto entry = makeUniqueNoThrow<AnnotationEntry>();
  if (!entry) {
    LOG_ERR("ANN", "OOM: annotation delete");
    return Result::IoError;
  }
  const Result loaded = load(bookPath, id, *entry);
  if (loaded != Result::Success) return loaded;
  char path[80];
  recordPath(path, sizeof(path), directoryPath(bookPath).c_str(), id);
  char backup[96];
  snprintf(backup, sizeof(backup), "%s.bak", path);
  // A stale backup must not resurrect a successfully deleted annotation.
  if ((Storage.exists(backup) && !Storage.remove(backup)) || !Storage.remove(path)) {
    LOG_ERR("ANN", "Could not delete %s", path);
    return Result::IoError;
  }
  return Result::Success;
}

AnnotationStore::Result AnnotationStore::list(const std::string& bookPath, AnnotationSummary* entries,
                                              const size_t capacity, size_t& count) {
  count = 0;
  if (!validPathText(bookPath) || !entries || capacity < MAX_ENTRIES) {
    LOG_ERR("ANN", "Invalid annotation list buffer or source");
    return Result::InvalidData;
  }
  if (!Storage.ready()) {
    LOG_ERR("ANN", "Annotation storage unavailable");
    return Result::IoError;
  }
  if (!Storage.exists(ROOT)) return Result::Success;
  {
    auto root = Storage.open(ROOT);
    if (!root || !root.isDirectory()) {
      LOG_ERR("ANN", "Could not open annotation root");
      return Result::IoError;
    }
  }
  const std::string directory = directoryPath(bookPath);
  if (!Storage.exists(directory.c_str())) return Result::Success;
  {
    auto sourceDirectory = Storage.open(directory.c_str());
    if (!sourceDirectory || !sourceDirectory.isDirectory()) {
      LOG_ERR("ANN", "Could not open annotation source directory");
      return Result::IoError;
    }
  }
  auto entry = makeUniqueNoThrow<AnnotationEntry>();
  if (!entry) {
    LOG_ERR("ANN", "OOM: annotation scan");
    return Result::IoError;
  }
  for (uint16_t id = 1; id <= MAX_ENTRIES; ++id) {
    const Result loaded = readEntry(directory.c_str(), bookPath, id, *entry);
    if (loaded == Result::NotFound) continue;
    if (loaded != Result::Success) {
      count = 0;
      return loaded;
    }
    auto& summary = entries[count++];
    summary.id = id;
    summary.kind = entry->kind;
    summary.spineIndex = entry->location.computedSpineIndex;
    summary.percentage = entry->location.percentage;
    summary.visibleTextOffset = entry->location.visibleTextOffset;
    summary.hasVisibleTextOffset = entry->location.hasVisibleTextOffset;
    const std::string label = annotationText(entry->note.empty() ? entry->excerpt : entry->note, sizeof(summary.label) - 1);
    memcpy(summary.label, label.c_str(), label.size() + 1);
  }
  std::sort(entries, entries + count, [](const AnnotationSummary& a, const AnnotationSummary& b) {
    if (a.spineIndex != b.spineIndex) return a.spineIndex < b.spineIndex;
    if (a.percentage != b.percentage) return a.percentage < b.percentage;
    const uint32_t aOffset = a.hasVisibleTextOffset ? a.visibleTextOffset : UINT32_MAX;
    const uint32_t bOffset = b.hasVisibleTextOffset ? b.visibleTextOffset : UINT32_MAX;
    if (aOffset != bOffset) return aOffset < bOffset;
    return a.id < b.id;
  });
  return Result::Success;
}
