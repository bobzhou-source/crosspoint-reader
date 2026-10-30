#include "StudyCardStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PersistableStore.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "AnnotationStore.h"

namespace {
constexpr char ROOT[] = "/.crosspoint/study";
constexpr size_t MAX_FILE_BYTES = 8192;
constexpr uint8_t VERSION = 1;
using Result = StudyCardStore::Result;

bool validPath(const std::string& path) {
  if (path.empty() || path.size() > AnnotationEntry::MAX_PATH_BYTES) return false;
  for (const unsigned char c : path) {
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

std::string directoryPath(const std::string& bookPath) {
  return std::string(ROOT) + "/" + AnnotationStore::sourceKey(bookPath);
}

bool readableDirectory(const char* path) {
  HalFile directory = Storage.open(path);
  if (directory && directory.isDirectory()) return true;
  LOG_ERR("STUDY", "Unreadable deck directory: %s", path);
  return false;
}

void recordPath(char* path, const size_t capacity, const char* directory, const uint16_t id) {
  snprintf(path, capacity, "%s/%03u.json", directory, static_cast<unsigned>(id));
}

bool readText(const JsonVariantConst value, std::string& target, const size_t limit) {
  const char* text = value.as<const char*>();
  if (!text || strnlen(text, limit + 1) > limit) return false;
  target = text;
  return true;
}

Result readCard(const char* directory, const std::string& bookPath, const uint16_t id, StudyCardEntry& card) {
  char path[80];
  recordPath(path, sizeof(path), directory, id);
  if (!Storage.exists(path)) return Result::NotFound;
  {
    HalFile file;
    if (!Storage.openFileForRead("STUDY", path, file)) return Result::IoError;
    if (file.size() == 0 || file.size() > MAX_FILE_BYTES) return Result::InvalidData;
  }
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path, doc)) return Result::IoError;
  auto& entry = card.source;
  const JsonObjectConst source = doc["source"].as<JsonObjectConst>();
  if (doc["version"] != VERSION || doc["id"] != id ||
      !readText(doc["front"], card.front, StudyCardEntry::MAX_FRONT_BYTES) || card.front.empty() ||
      !source["id"].is<uint16_t>() || source["id"].as<uint16_t>() == 0 ||
      source["id"].as<uint16_t>() > AnnotationStore::MAX_ENTRIES || !source["kind"].is<uint8_t>() ||
      source["kind"].as<uint8_t>() > static_cast<uint8_t>(AnnotationKind::ReviewLater) ||
      !readText(source["sourcePath"], entry.sourcePath, AnnotationEntry::MAX_PATH_BYTES) || entry.sourcePath != bookPath ||
      !readText(source["sourceTitle"], entry.sourceTitle, AnnotationEntry::MAX_TITLE_BYTES) ||
      !readText(source["chapterTitle"], entry.chapterTitle, AnnotationEntry::MAX_TITLE_BYTES) ||
      !readText(source["chapterHref"], entry.chapterHref, AnnotationEntry::MAX_PATH_BYTES) ||
      !readText(source["xpath"], entry.location.xpath, AnnotationEntry::MAX_PATH_BYTES) ||
      !readText(source["excerpt"], entry.excerpt, AnnotationEntry::MAX_EXCERPT_BYTES) || entry.excerpt.empty() ||
      !readText(source["note"], entry.note, AnnotationEntry::MAX_NOTE_BYTES) ||
      !source["si"].is<uint16_t>() || !source["pc"].is<uint16_t>() || !source["pp"].is<uint16_t>() ||
      !source["percentage"].is<float>() || !source["pageContext"].is<bool>() || !source["createdAt"].is<uint64_t>() ||
      (!source["vo"].isNull() && !source["vo"].is<uint32_t>())) {
    LOG_ERR("STUDY", "Invalid card: %s", path);
    return Result::InvalidData;
  }
  entry.location.percentage = source["percentage"].as<float>();
  if (!validPath(entry.sourcePath) || !validPath(entry.chapterHref) || !validPath(entry.location.xpath) ||
      !std::isfinite(entry.location.percentage) || entry.location.percentage < 0 || entry.location.percentage > 1) {
    LOG_ERR("STUDY", "Invalid source location: %s", path);
    return Result::InvalidData;
  }
  card.id = id;
  entry.id = source["id"].as<uint16_t>();
  entry.kind = static_cast<AnnotationKind>(source["kind"].as<uint8_t>());
  entry.location.computedSpineIndex = source["si"].as<uint16_t>();
  entry.location.computedChapterPageCount = source["pc"].as<uint16_t>();
  entry.location.computedChapterProgress = source["pp"].as<uint16_t>();
  entry.location.hasVisibleTextOffset = !source["vo"].isNull();
  entry.location.visibleTextOffset = source["vo"] | static_cast<uint32_t>(0);
  // Native annotation persistence does not populate bookmark-only display fields.
  entry.location.name.clear();
  entry.location.summary.clear();
  entry.pageContext = source["pageContext"].as<bool>();
  entry.createdAt = source["createdAt"].as<uint64_t>();
  return Result::Success;
}

bool writeCard(const char* path, const StudyCardEntry& card, const uint16_t id) {
  JsonDocument doc;
  doc["version"] = VERSION;
  doc["id"] = id;
  doc["front"] = card.front;
  auto source = doc["source"].to<JsonObject>();
  const auto& entry = card.source;
  source["id"] = entry.id;
  source["kind"] = static_cast<uint8_t>(entry.kind);
  source["sourcePath"] = entry.sourcePath;
  source["sourceTitle"] = entry.sourceTitle;
  source["chapterTitle"] = entry.chapterTitle;
  source["chapterHref"] = entry.chapterHref;
  source["xpath"] = entry.location.xpath;
  source["percentage"] = entry.location.percentage;
  source["si"] = entry.location.computedSpineIndex;
  source["pc"] = entry.location.computedChapterPageCount;
  source["pp"] = entry.location.computedChapterProgress;
  if (entry.location.hasVisibleTextOffset) source["vo"] = entry.location.visibleTextOffset;
  source["excerpt"] = entry.excerpt;
  source["note"] = entry.note;
  source["pageContext"] = entry.pageContext;
  source["createdAt"] = entry.createdAt;
  if (doc.overflowed() || measureJson(doc) > MAX_FILE_BYTES) {
    LOG_ERR("STUDY", "Card exceeded memory or size budget");
    return false;
  }
  char temporary[96];
  snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  bool written;
  {
    HalFile file;
    if (!Storage.openFileForWrite("STUDY", temporary, file)) return false;
    written = serializeJson(doc, file) == measureJson(doc);
    file.flush();
    written = file.close() && written;  // Close before renaming.
  }
  // Cards are immutable snapshots: only a free slot is written, never replaced.
  if (!written || Storage.exists(path) || !Storage.rename(temporary, path)) {
    Storage.remove(temporary);
    LOG_ERR("STUDY", "Could not commit card: %s", path);
    return false;
  }
  return true;
}
}  // namespace

StudyCardStore::Result StudyCardStore::create(const std::string& bookPath, const uint16_t annotationId,
                                             const char* fallbackQuestion, StudyCardEntry& card) {
  card.id = 0;
  const auto loaded = AnnotationStore::load(bookPath, annotationId, card.source);
  if (loaded != AnnotationStore::Result::Success) {
    LOG_ERR("STUDY", "Could not load saved source annotation");
    if (loaded == AnnotationStore::Result::NotFound) return Result::NotFound;
    return loaded == AnnotationStore::Result::IoError ? Result::IoError : Result::InvalidData;
  }
  if (card.source.excerpt.empty()) {
    LOG_ERR("STUDY", "Saved source annotation has no context");
    return Result::InvalidData;
  }
  card.front = annotationText(card.source.kind == AnnotationKind::Question && !card.source.note.empty()
                                  ? card.source.note
                                  : (fallbackQuestion ? fallbackQuestion : ""),
                              StudyCardEntry::MAX_FRONT_BYTES);
  if (card.front.empty()) return Result::InvalidData;
  const std::string directory = directoryPath(bookPath);
  if (!Storage.ready() || !Storage.mkdir(directory.c_str())) return Result::IoError;
  if (!readableDirectory(directory.c_str())) return Result::IoError;
  // Validate the existing deck before adding; a damaged deck is not a successful empty deck.
  auto previous = makeUniqueNoThrow<StudyCardEntry>();
  if (!previous) {
    LOG_ERR("STUDY", "OOM: card validation");
    return Result::IoError;
  }
  uint16_t freeId = 0;
  for (uint16_t id = 1; id <= MAX_ENTRIES; ++id) {
    const Result status = readCard(directory.c_str(), bookPath, id, *previous);
    if (status == Result::NotFound) {
      if (freeId == 0) freeId = id;
    } else if (status != Result::Success) {
      return status;
    }
  }
  if (freeId == 0) return Result::LimitReached;
  previous.reset();
  char path[80];
  recordPath(path, sizeof(path), directory.c_str(), freeId);
  if (!writeCard(path, card, freeId)) return Result::IoError;
  card.id = freeId;
  return Result::Success;
}

StudyCardStore::Result StudyCardStore::load(const std::string& bookPath, const uint16_t id, StudyCardEntry& card) {
  if (!validPath(bookPath) || id == 0 || id > MAX_ENTRIES) return Result::InvalidData;
  if (!Storage.ready()) return Result::IoError;
  return readCard(directoryPath(bookPath).c_str(), bookPath, id, card);
}

StudyCardStore::Result StudyCardStore::list(const std::string& bookPath, StudyCardSummary* entries,
                                           const size_t capacity, size_t& count) {
  count = 0;
  if (!validPath(bookPath) || !entries || capacity < MAX_ENTRIES) return Result::InvalidData;
  if (!Storage.ready()) return Result::IoError;
  if (Storage.exists(ROOT) && !readableDirectory(ROOT)) return Result::IoError;
  const std::string directory = directoryPath(bookPath);
  if (!Storage.exists(directory.c_str())) return Result::Success;
  if (!readableDirectory(directory.c_str())) return Result::IoError;
  auto card = makeUniqueNoThrow<StudyCardEntry>();
  if (!card) {
    LOG_ERR("STUDY", "OOM: card list");
    return Result::IoError;
  }
  for (uint16_t id = 1; id <= MAX_ENTRIES; ++id) {
    const Result loaded = readCard(directory.c_str(), bookPath, id, *card);
    if (loaded == Result::NotFound) continue;
    if (loaded != Result::Success) {
      count = 0;
      return loaded;
    }
    auto& summary = entries[count++];
    summary.id = id;
    summary.kind = card->source.kind;
    summary.spineIndex = card->source.location.computedSpineIndex;
    const std::string front = annotationText(card->front, sizeof(summary.front) - 1);
    memcpy(summary.front, front.c_str(), front.size() + 1);
  }
  return Result::Success;
}

StudyCardStore::Result StudyCardStore::remove(const std::string& bookPath, const uint16_t id) {
  auto card = makeUniqueNoThrow<StudyCardEntry>();
  if (!card) {
    LOG_ERR("STUDY", "OOM: card deletion");
    return Result::IoError;
  }
  const Result loaded = load(bookPath, id, *card);
  if (loaded != Result::Success) return loaded;
  char path[80];
  recordPath(path, sizeof(path), directoryPath(bookPath).c_str(), id);
  return Storage.remove(path) ? Result::Success : Result::IoError;
}
