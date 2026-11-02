#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <iterator>

void RecentBooksStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : recentBooks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
  }
}

bool RecentBooksStore::fromJson(JsonVariantConst doc) {
  // Tolerate a missing/invalid 'books' key (treat as empty list); only a
  // JSON parse error is fatal. A null JsonArray iterates zero times.
  ++revision;
  recentBooks.clear();
  JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  recentBooks.reserve(std::min(arr.size(), static_cast<size_t>(MAX_RECENT_BOOKS)));
  for (JsonObjectConst obj : arr) {
    if (getCount() >= MAX_RECENT_BOOKS) break;
    RecentBook book;
    book.path = obj["path"] | "";
    book.title = obj["title"] | "";
    book.author = obj["author"] | "";
    book.coverBmpPath = obj["coverBmpPath"] | "";
    recentBooks.push_back(book);
  }

  LOG_DBG("RBS", "Recent books loaded from file (%d entries)", getCount());
  return true;
}

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  // Drop stale entries first so a new add can't evict a valid book in their stead.
  pruneMissing();

  // Remove existing entry if present
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    recentBooks.erase(it);
  }

  // Add to front
  recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath});

  // Trim to max size
  if (recentBooks.size() > MAX_RECENT_BOOKS) {
    recentBooks.resize(MAX_RECENT_BOOKS);
  }
  ++revision;

  saveToFile();
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    RecentBook& book = *it;
    book.title = title;
    book.author = author;
    book.coverBmpPath = coverBmpPath;
    ++revision;
    saveToFile();
  }
}

bool RecentBooksStore::removeByPath(const std::string& path) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  recentBooks.erase(it);
  ++revision;
  if (!saveToFile()) {
    LOG_ERR("RBS", "Failed to persist removal of recent book: %s", path.c_str());
  }
  return true;
}

void RecentBooksStore::updatePath(const std::string& oldPath, const std::string& newPath,
                                  const std::string& oldCachePath, const std::string& newCachePath) {
  auto it = std::find_if(recentBooks.begin(), recentBooks.end(),
                         [&](const RecentBook& book) { return book.path == oldPath; });
  if (it == recentBooks.end()) {
    return;
  }
  it->path = newPath;
  if (!oldCachePath.empty() && !it->coverBmpPath.empty() && it->coverBmpPath.rfind(oldCachePath, 0) == 0) {
    it->coverBmpPath = newCachePath + it->coverBmpPath.substr(oldCachePath.size());
  }
  ++revision;
  saveToFile();
}

bool RecentBooksStore::isMissing(const RecentBook& book) { return !Storage.exists(book.path.c_str()); }

uint16_t RecentBooksStore::missingMask() const {
  static_assert(MAX_RECENT_BOOKS <= 16, "Missing-book mask must cover every recent entry");
  uint16_t mask = 0;
  for (size_t i = 0; i < recentBooks.size(); ++i) {
    if (isMissing(recentBooks[i])) mask |= static_cast<uint16_t>(1u << i);
  }
  return mask;
}

bool RecentBooksStore::pruneMissing(uint16_t mask) {
  if (mask == 0) return false;
  size_t kept = 0;
  for (size_t i = 0; i < recentBooks.size(); ++i) {
    if ((mask & (1u << i)) != 0) continue;
    if (kept != i) recentBooks[kept] = std::move(recentBooks[i]);
    ++kept;
  }
  if (kept == recentBooks.size()) return false;
  recentBooks.resize(kept);
  ++revision;
  return true;
}

bool RecentBooksStore::pruneMissing() { return pruneMissing(missingMask()); }

RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) {
    lastBookFileName = path.substr(lastSlash + 1);
  }

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());

  // If epub, try to load the metadata for title/author and cover.
  // Use buildIfMissing=false to avoid heavy epub loading on boot; getTitle()/getAuthor() may be
  // blank until the book is opened, and entries with missing title are omitted from recent list.
  if (FsHelpers::hasEpubExtension(lastBookFileName)) {
    Epub epub(path, "/.crosspoint");
    epub.load(false, true);
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath()};
  } else if (FsHelpers::hasXtcExtension(lastBookFileName)) {
    // Handle XTC file
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      return RecentBook{path, xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath()};
    }
  } else if (FsHelpers::hasTxtExtension(lastBookFileName) || FsHelpers::hasMarkdownExtension(lastBookFileName)) {
    return RecentBook{path, lastBookFileName, "", ""};
  }
  return RecentBook{path, "", "", ""};
}
