#include "SurpriseMeSource.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <string_view>

#include "BookProgress.h"
#include "ConceptEngine.h"
#include "SurpriseMe.h"

namespace surprise {
namespace {
constexpr const char* INBOX_DIR = "/books/Inbox";
constexpr size_t MAX_CANDIDATES = 48;
constexpr size_t MAX_INBOX_SCAN = 40;
constexpr unsigned MAX_LIBRARY_ROWS = 12;
constexpr size_t NAME_BUFFER_SIZE = 256;

// What ReaderActivity can open. Bitmaps are a viewer, not a book.
bool supportedBook(std::string_view name) {
  return FsHelpers::hasEpubExtension(name) || FsHelpers::hasXtcExtension(name) || FsHelpers::hasTxtExtension(name) ||
         FsHelpers::hasMarkdownExtension(name) || FsHelpers::hasFb2Extension(name) || FsHelpers::hasHtmlExtension(name);
}

struct Pool {
  std::string paths[MAX_CANDIDATES];
  Candidate items[MAX_CANDIDATES];
  size_t count = 0;

  bool contains(const std::string& path) const {
    for (size_t i = 0; i < count; ++i)
      if (paths[i] == path) return true;
    return false;
  }

  void add(const std::string& path, const Origin origin, const bool shortRead) {
    if (count >= MAX_CANDIDATES || contains(path)) return;
    paths[count] = path;
    items[count] = Candidate{paths[count].c_str(), origin, loadBookProgress(path), shortRead};
    ++count;
  }
};

void scanInbox(Pool& pool) {
  auto dir = Storage.open(INBOX_DIR);
  if (!dir || !dir.isDirectory()) return;
  dir.rewindDirectory();
  auto name = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!name) {
    LOG_ERR("SRP", "OOM: inbox name buffer");
    dir.close();
    return;
  }
  size_t seen = 0;
  for (auto file = dir.openNextFile(); file && seen < MAX_INBOX_SCAN; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name.get(), NAME_BUFFER_SIZE);
    file.close();
    const std::string_view entry(name.get());
    if (entry.empty() || entry[0] == '.' || !supportedBook(entry)) continue;
    ++seen;
    pool.add(std::string(INBOX_DIR) + "/" + std::string(entry), Origin::Inbox, true);
  }
  dir.close();
}

void scanLibrary(Pool& pool) {
  library::LibraryIndexFile index;
  if (!index.open(library::libraryIndexPath())) return;
  const unsigned rows = std::min<unsigned>(index.bookCount(), MAX_LIBRARY_ROWS);
  library::ClixRecord record{};
  std::string path;
  for (unsigned row = 0; row < rows && pool.count < MAX_CANDIDATES; ++row) {
    const uint16_t ordinal = index.ordinalForRow(library::SortOrder::RecentDesc, static_cast<uint16_t>(row));
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path)) continue;
    pool.add(path, Origin::Library, false);
  }
  index.close();
}

// Fills conceptDot for the Inbox items that inbox-concepts.idx describes.
void scoreInboxConcepts(Pool& pool, const int8_t* contextVec) {
  if (!Storage.exists(ConceptEngine::SHELF_PATH)) return;
  int8_t context[ConceptEngine::DIM];
  if (!contextVec) {
    // The page read last at length this session, else the newest episodic page.
    if (!ConceptEngine::readingContext(context)) return;
    contextVec = context;
  }
  uint64_t hashes[MAX_CANDIDATES];
  int32_t dots[MAX_CANDIDATES];
  size_t slots[MAX_CANDIDATES];
  size_t inbox = 0;
  for (size_t i = 0; i < pool.count; ++i) {
    if (pool.items[i].origin != Origin::Inbox) continue;
    hashes[inbox] = ConceptEngine::fnv1a64(pool.paths[i]);
    slots[inbox++] = i;
  }
  if (inbox == 0 || ConceptEngine::scoreShelf(contextVec, hashes, inbox, dots) == 0) return;
  // scoreShelf reports a missing record as INT32_MIN, which is NO_CONCEPT.
  for (size_t i = 0; i < inbox; ++i) pool.items[slots[i]].conceptDot = dots[i];
}
}  // namespace

bool findBook(std::string& outPath, const int8_t* currentContextVec) {
  auto pool = makeUniqueNoThrow<Pool>();
  if (!pool) {
    LOG_ERR("SRP", "OOM: surprise candidates");
    return false;
  }
  // Inbox first: those are the short reads a reader can finish in one sitting.
  scanInbox(*pool);
  scanLibrary(*pool);
  scoreInboxConcepts(*pool, currentContextVec);
  const char* chosen = pick(pool->items, pool->count, esp_random());
  if (!chosen) return false;
  outPath = chosen;
  LOG_INF("SRP", "Surprise pick: %s", outPath.c_str());
  return true;
}

}  // namespace surprise
