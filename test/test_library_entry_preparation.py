"""Exercise production Library entry preparation and the recents pruning split.

Run: python3 test/test_library_entry_preparation.py (requires a host C++ compiler).
No device, panel, SD-card image, or firmware build is used.
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
library_source = (ROOT / "src/activities/library/LibraryListActivity.cpp").read_text()
recents_source = (ROOT / "src/RecentBooksStore.cpp").read_text()
recents_header = (ROOT / "src/RecentBooksStore.h").read_text()


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


recents_methods = "\n".join(method(recents_source, signature) for signature in (
    "void RecentBooksStore::addBook(",
    "void RecentBooksStore::updateBook(",
    "bool RecentBooksStore::removeByPath(",
    "void RecentBooksStore::updatePath(",
    "bool RecentBooksStore::isMissing(",
    "uint16_t RecentBooksStore::missingMask(",
    "bool RecentBooksStore::pruneMissing(uint16_t",
    "bool RecentBooksStore::pruneMissing()",
))
library_methods = "\n".join(method(library_source, signature) for signature in (
    "void LibraryListActivity::prepareEnter(",
    "void LibraryListActivity::loadEntryIndex(",
    "void LibraryListActivity::onEnter(",
))
capacity_line = next(line.strip() for line in recents_header.splitlines() if "static constexpr int MAX_RECENT_BOOKS" in line)

harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include <BenchmarkBuffer.h>
#define LOG_ERR(...) ((void)0)
bool indexActive = false, renderLocked = false;
struct StorageDouble {
  std::set<std::string> paths;
  int stats = 0;
  bool exists(const char* path) { ++stats; return paths.count(path) != 0; }
} Storage;
struct RecentBook { std::string path, title, author, coverBmpPath; };
struct RecentBooksStore {
''' + capacity_line + r'''
  std::vector<RecentBook> recentBooks;
  uint32_t revision = 0;
  int saves = 0;
  int getCount() const { return static_cast<int>(recentBooks.size()); }
  uint32_t getRevision() const { return revision; }
  const std::vector<RecentBook>& getBooks() const { return recentBooks; }
  bool saveToFile() { assert(!indexActive); ++saves; return true; }
  void addBook(const std::string&, const std::string&, const std::string&, const std::string&);
  void updateBook(const std::string&, const std::string&, const std::string&, const std::string&);
  bool removeByPath(const std::string&);
  void updatePath(const std::string&, const std::string&, const std::string&, const std::string&);
  static bool isMissing(const RecentBook&);
  uint16_t missingMask() const;
  bool pruneMissing(uint16_t);
  bool pruneMissing();
} RECENT_BOOKS;
''' + recents_methods + r'''
namespace benchmark {
int spans = 0;
uint32_t screen = 0, rows = 0;
uint64_t start() { return 1; }
void span(EventType type, uint64_t started, uint32_t s, uint32_t hit, uint32_t r) {
  assert(type == EventType::NavRows && started == 1 && hit == 0);
  ++spans; screen = s; rows = r;
}
}
namespace library {
bool dirty = false;
const char* libraryIndexPath() { return "index"; }
bool isLibraryIndexDirty() { return dirty; }
}
struct { int libraryUseMetadata = 1; } SETTINGS;
struct Index {
  bool opened = false, available = true, ranksBad = false, dedupBad = false;
  int opens = 0, closes = 0;
  struct Header { int metadataEnabled = 1; } metadata;
  bool open(const char*) { ++opens; opened = available; indexActive = opened; return opened; }
  void close() { ++closes; opened = false; indexActive = false; }
  ~Index() { close(); }
  bool isOpen() const { return opened; }
  bool ranksDegraded() const { return ranksBad; }
  bool dedupDegraded() const { return dedupBad; }
  const Header& header() const { return metadata; }
  unsigned bookCount() const { return opened ? 1680 : 0; }
};
struct MappedInputManager {
  enum class Button { Confirm };
  bool held = false;
  bool isPressed(Button) const { return held; }
};
struct RenderLock {
  template<class T> explicit RenderLock(T&) { assert(!renderLocked); renderLocked = true; }
  ~RenderLock() { renderLocked = false; }
};
struct UiTabListActivity { void onEnter() { assert(renderLocked); } };
uint32_t millis() { return 42; }
struct LibraryListActivity : UiTabListActivity {
  Index index;
  MappedInputManager mappedInput;
  enum class EntryPreparation : uint8_t { None, NeedsPrune, Ready };
  EntryPreparation entryPreparation = EntryPreparation::None;
  uint16_t preparedMissingMask = 0;
  uint32_t preparedRecentsRevision = 0;
  bool buildPending = false, buildFailed = false, buildHeld = false, degraded = false;
  uint8_t buildRounds = 0;
  uint32_t enteredMs = 0;
  std::atomic<bool> firstPainted{false};
  bool lockNextConfirmRelease = false;
  int pinScans = 0, updates = 0, flagSyncs = 0;
  std::vector<std::string> pinnedPaths;
  struct App { template<class F, class T> void on(int, F, T*) {} } app;
  static constexpr int ACTION_SEARCH = 1, ACTION_REBUILD = 2, ACTION_BACK = 3;
  static void searchActionTrampoline() {}
  static void rebuildActionTrampoline() {}
  static void backActionTrampoline() {}
  void syncRebuildFlag() { ++flagSyncs; }
  void requestUpdate(bool immediate) { assert(renderLocked && immediate); ++updates; }
  void resolvePinned() {
    ++pinScans;
    pinnedPaths.clear();
    for (const auto& book : RECENT_BOOKS.getBooks()) pinnedPaths.push_back(book.path);
  }
  void prepareEnter();
  void loadEntryIndex();
  void onEnter();
};
''' + library_methods + r'''
void reset(std::initializer_list<const char*> recent, std::initializer_list<const char*> present) {
  assert(!indexActive);
  RECENT_BOOKS.recentBooks.clear();
  RECENT_BOOKS.revision = 0;
  RECENT_BOOKS.saves = 0;
  for (const auto* path : recent) RECENT_BOOKS.recentBooks.push_back({path, "Title", "Author", "cover"});
  Storage.paths.clear();
  for (const auto* path : present) Storage.paths.insert(path);
  Storage.stats = 0;
  benchmark::spans = 0;
  library::dirty = false;
}
int main() {
  reset({"/a", "/b"}, {"/a", "/b"});
  {
    LibraryListActivity activity;
    activity.prepareEnter();
    assert(Storage.stats == 2 && RECENT_BOOKS.saves == 0 && RECENT_BOOKS.getCount() == 2);
    assert(activity.index.opens == 1 && activity.pinScans == 1 && activity.updates == 0);
    assert(activity.flagSyncs == 0 && !activity.firstPainted);
    assert(benchmark::spans == 1 && benchmark::rows == 1680);
    assert(benchmark::screen == static_cast<uint32_t>(benchmark::NavRowsScreen::LibraryPrepare));
    activity.prepareEnter();
    assert(Storage.stats == 2 && activity.index.opens == 1 && benchmark::spans == 1);
    activity.onEnter();
    assert(Storage.stats == 2 && activity.index.opens == 1 && activity.pinScans == 1);
    assert(activity.entryPreparation == LibraryListActivity::EntryPreparation::None);
    assert(activity.updates == 1 && activity.flagSyncs == 1 && activity.enteredMs == 42);
    activity.index.close();
    activity.onEnter(); // consumed preparation cannot mask a later real entry
    assert(Storage.stats == 4 && activity.index.opens == 2 && activity.pinScans == 2);
  }
  reset({"/missing", "/b", "/also-missing"}, {"/b"});
  {
    LibraryListActivity activity;
    activity.prepareEnter();
    assert(activity.index.opens == 0 && activity.pinScans == 0 && Storage.stats == 3);
    assert(RECENT_BOOKS.getCount() == 3 && RECENT_BOOKS.saves == 0 && RECENT_BOOKS.getRevision() == 0);
    activity.onEnter();
    assert(Storage.stats == 3 && RECENT_BOOKS.saves == 1 && RECENT_BOOKS.getCount() == 1);
    assert(activity.index.opens == 1 && activity.pinScans == 1);
    assert(activity.pinnedPaths == std::vector<std::string>{"/b"});
  }
  reset({"/a", "/b"}, {"/a", "/b", "/new"});
  {
    LibraryListActivity activity;
    activity.prepareEnter();
    // Defensive invalidation: a changed store must never reuse stale pins.
    RECENT_BOOKS.recentBooks[0].path = "/new";
    ++RECENT_BOOKS.revision;
    activity.onEnter();
    assert(activity.index.closes == 1 && activity.index.opens == 2 && activity.pinScans == 2);
    assert(Storage.stats == 4 && activity.pinnedPaths[0] == "/new");
  }
  reset({"/missing", "/b"}, {"/b"});
  {
    LibraryListActivity activity;
    activity.prepareEnter();
    std::swap(RECENT_BOOKS.recentBooks[0], RECENT_BOOKS.recentBooks[1]);
    ++RECENT_BOOKS.revision;
    activity.onEnter();
    assert(Storage.stats == 4 && RECENT_BOOKS.getCount() == 1 && RECENT_BOOKS.getBooks()[0].path == "/b");
  }
  reset({}, {});
  {
    LibraryListActivity activity;
    activity.index.available = false;
    activity.prepareEnter();
    activity.onEnter();
    assert(activity.index.opens == 1 && activity.buildPending && Storage.stats == 0);
    assert(benchmark::rows == 0 && activity.pinScans == 1);
  }
  reset({}, {});
  {
    LibraryListActivity activity;
    library::dirty = true;
    activity.mappedInput.held = true;
    activity.onEnter(); // physical/cover-grid launch has no preparation
    assert(activity.index.opens == 1 && activity.pinScans == 1 && activity.buildPending);
    assert(activity.lockNextConfirmRelease && benchmark::spans == 0);
  }
  reset({}, {});
  for (int i = 0; i < RecentBooksStore::MAX_RECENT_BOOKS; ++i) {
    const std::string path = "/book" + std::to_string(i);
    RECENT_BOOKS.recentBooks.push_back({path, "", "", ""});
    if (i != RecentBooksStore::MAX_RECENT_BOOKS - 1) Storage.paths.insert(path);
  }
  const uint16_t mask = RECENT_BOOKS.missingMask();
  assert(mask == (1u << (RecentBooksStore::MAX_RECENT_BOOKS - 1)));
  const int stats = Storage.stats;
  assert(RECENT_BOOKS.pruneMissing(mask));
  assert(Storage.stats == stats && RECENT_BOOKS.getCount() == RecentBooksStore::MAX_RECENT_BOOKS - 1);
  reset({"/a"}, {"/a", "/b", "/new"});
  auto revision = RECENT_BOOKS.getRevision();
  RECENT_BOOKS.updateBook("/a", "changed", "", "");
  assert(RECENT_BOOKS.getRevision() != revision);
  revision = RECENT_BOOKS.getRevision();
  RECENT_BOOKS.addBook("/b", "", "", "");
  assert(RECENT_BOOKS.getRevision() != revision);
  revision = RECENT_BOOKS.getRevision();
  RECENT_BOOKS.updatePath("/b", "/new", "", "");
  assert(RECENT_BOOKS.getRevision() != revision);
  revision = RECENT_BOOKS.getRevision();
  assert(RECENT_BOOKS.removeByPath("/new") && RECENT_BOOKS.getRevision() != revision);
  puts("Library preparation: private/read-only, one-shot, no duplicate I/O, missing/stale recents safe");
}
'''

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "library_entry_preparation.cpp"
    executable = Path(directory) / "library_entry_preparation"
    cpp.write_text(harness)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "lib/Benchmark"), str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=30)
