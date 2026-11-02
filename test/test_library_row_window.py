"""Exercise production Library row materialization with counted index I/O.

Run: python3 test/test_library_row_window.py (requires a host C++ compiler).
The index/UI doubles isolate the activity's cache and invalidation behavior;
LibraryIndexFile's binary I/O is covered by test/library_index_file.
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/library/LibraryListActivity.cpp").read_text()


def method(signature):
    start = source.index(signature)
    end = source.index("\n}", start) + 2
    return source[start:end]


methods = [
    "constexpr bool isDescending(",
    "constexpr bool isRecentSort(",
    "constexpr bool isAuthorSort(",
    "constexpr library::SortOrder orderForTab(",
    "int LibraryListActivity::pinnedCount(",
    "void LibraryListActivity::resolvePinned(",
    "void LibraryListActivity::refreshOverlap(",
    "void LibraryListActivity::resetAfterRebuild(",
    "void LibraryListActivity::selectTab(",
    "int LibraryListActivity::bookRowCount(",
    "int LibraryListActivity::rowFor(",
    "uint32_t LibraryListActivity::titleInitialFor(",
    "void LibraryListActivity::restoreExpandedList(",
    "void LibraryListActivity::applyFilter(",
    "bool LibraryListActivity::rowTextFor(",
    "void LibraryListActivity::refreshRowWindow(",
    "void LibraryListActivity::formatInitialHeading(",
    "void LibraryListActivity::formatAuthorHeading(",
]
# These are intentionally one-line production methods.
methods_source = "\n\n".join(method(s) for s in methods)
for signature in ("int LibraryListActivity::listCount(",):
    start = source.index(signature)
    methods_source += "\n" + source[start:source.index("\n", start)]

harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <BenchmarkBuffer.h>
namespace benchmark {
bool recording = false;
int clocks = 0, spans = 0;
uint32_t lastScreen = 0, lastHit = 0, lastRows = 0;
uint64_t start() { return recording ? ++clocks : 0; }
void span(EventType type, uint64_t started, uint32_t screen, uint32_t hit, uint32_t rows) {
  if (!started) return;
  assert(type == EventType::NavRows);
  ++spans; lastScreen = screen; lastHit = hit; lastRows = rows;
}
}
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define tr(key) #key
constexpr int RECENT_TAB = 0, TITLE_TAB = 1, AUTHOR_TAB = 2, TAB_SLOTS = 3;
namespace library {
enum class SortOrder { RecentAsc, RecentDesc, TitleAsc, TitleDesc, AuthorAsc, AuthorDesc };
struct ClixRecord { int ordinal = 0; char fold[96]{}; uint8_t foldLen = 0; };
struct BookIdentity { uint64_t pathHash; uint32_t fileSize; };
uint64_t clixPathHash(const char* p, size_t n) { return std::hash<std::string_view>{}({p, n}); }
uint32_t foldedGroupInitial(std::string_view s) { return s.empty() ? 0 : static_cast<uint8_t>(s[0]); }
std::string fold(const std::string& s) { return s; }
bool matchesQuery(std::string_view s, std::string_view q) { return s.find(q) != s.npos; }
}
struct Row { std::string title, author, name, path; };
struct Index {
  std::vector<Row> rows;
  int reads = 0, records = 0;
  bool failed = false;
  bool isOpen() const { return true; }
  bool ioFailed() const { return failed; }
  size_t bookCount() const { return rows.size(); }
  uint16_t ordinalForRow(library::SortOrder order, uint16_t row) {
    ++reads;
    if (row >= rows.size()) return 0xFFFF;
    return order == library::SortOrder::TitleDesc || order == library::SortOrder::AuthorDesc ||
                   order == library::SortOrder::RecentDesc ? rows.size() - row - 1 : row;
  }
  bool readRecord(uint16_t ordinal, library::ClixRecord& out) {
    ++reads; ++records;
    if (failed || ordinal >= rows.size()) return false;
    out.ordinal = ordinal;
    snprintf(out.fold, sizeof(out.fold), "%s", rows[ordinal].title.c_str());
    out.foldLen = strlen(out.fold);
    return true;
  }
  bool readTitle(const library::ClixRecord& r, std::string& out) { ++reads; out = rows[r.ordinal].title; return true; }
  bool readAuthor(const library::ClixRecord& r, std::string& out) { ++reads; out = rows[r.ordinal].author; return true; }
  bool readName(const library::ClixRecord& r, std::string& out) { ++reads; out = rows[r.ordinal].name; return true; }
  bool recentRowsFor(const library::BookIdentity* identities, int count, uint16_t* output) {
    ++reads;
    for (int i = 0; i < count; ++i) {
      output[i] = 0xFFFF;
      for (size_t j = 0; j < rows.size(); ++j) {
        if (identities[i].pathHash == library::clixPathHash(rows[j].path.data(), rows[j].path.size())) output[i] = j;
      }
    }
    return true;
  }
};
struct RecentBook { std::string path, title, author; };
struct RecentBooksStore {
  static constexpr int MAX_RECENT_BOOKS = 10;
  std::vector<RecentBook> books;
  const auto& getBooks() const { return books; }
} RECENT_BOOKS;
struct HalFile { uint32_t fileSize() const { return 0; } };
struct StorageStub { bool openFileForRead(const char*, const char*, HalFile&) { return false; } } Storage;
template<class T> std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  return std::unique_ptr<T>(new typename std::remove_extent<T>::type[count]);
}
void utf8AppendCodepoint(uint32_t c, std::string& out) { assert(c < 128); out.push_back(static_cast<char>(c)); }
struct UITheme { static int getFileIcon(const std::string& name) { return name.find(".txt") != name.npos ? 2 : 1; } };
int listIconFor(int icon, int) { return icon; }
namespace fui {
struct ListItem { const char* label = nullptr; const char* subtitle = nullptr; const char* sectionHeading = nullptr;
  int icon = 0; int16_t actionValue = 0; };
struct ListNav { int selected = 0, top = 0; bool followOnBuild = false; };
}
struct LibraryListActivity {
  Index index;
  int activeTabIndex = 0;
  library::SortOrder sortOrder = library::SortOrder::RecentDesc;
  uint8_t descendingTabs = 1;
  std::string query, headerSearchTitle;
  std::unique_ptr<uint16_t[]> filtered;
  uint16_t filteredCount = 0;
  bool filterFailed = false;
  std::unique_ptr<uint16_t[]> groupStarts;
  uint16_t groupCount = 0;
  bool groupsCollapsed = false;
  fui::ListNav nav, expandedNav;
  std::vector<fui::ListItem> winItems;
  std::vector<std::string> winTitles, winAuthors, winHeaders;
  int winStart = -1;
  size_t winCapacity = 0;
  int winCount = 0;
  bool winCollapsed = false;
  uint16_t pinnedAscRows[10]{}, overlapRows[10]{};
  uint8_t pinnedTotal = 0, overlapCount = 0;
  int updates = 0;
  fui::ListNav& activeNav() { return nav; }
  int activeTab() const { return activeTabIndex; }
  void requestUpdate() { ++updates; }
  int pinnedCount() const;
  void resolvePinned();
  void refreshOverlap();
  void resetAfterRebuild();
  void selectTab(int, bool);
  int bookRowCount() const;
  int listCount() const;
  int rowFor(int) const;
  uint32_t titleInitialFor(int);
  void restoreExpandedList();
  void applyFilter();
  bool rowTextFor(int, std::string&, std::string&, std::string* = nullptr, uint32_t* = nullptr);
  void refreshRowWindow(int, size_t);
  static void formatInitialHeading(uint32_t, std::string&);
  void formatAuthorHeading(const std::string&, std::string&) const;
};
''' + methods_source + r'''
int main() {
  LibraryListActivity a;
  for (int i = 0; i < 8; ++i) {
    const std::string title(1, static_cast<char>('a' + i));
    a.index.rows.push_back({title, "Author " + title, title + (i % 2 ? ".txt" : ".epub"), "/" + title});
  }
  a.selectTab(TITLE_TAB, false);
  a.refreshRowWindow(0, 3);
  assert(a.index.reads == 15 && a.index.records == 3); // one record, not two, per Title row
  assert(a.winItems.size() == 3 && std::string(a.winItems[0].label) == "a");
  assert(std::string(a.winItems[0].sectionHeading) == "A" && a.winItems[1].icon == 2);
  const char* label = a.winItems[0].label;
  const int firstReads = a.index.reads;
  a.nav.selected = 2;
  a.nav.top = 0;
  for (int repaint = 0; repaint < 20; ++repaint) a.refreshRowWindow(0, 3);
  assert(a.index.reads == firstReads && a.winItems[0].label == label);
  assert(a.nav.selected == 2 && a.nav.top == 0); // selection/status repaints never move the viewport
  printf("Title window: cold=%d index operations, 20 same-window repaints=0\n", firstReads);

  a.refreshRowWindow(3, 3);
  assert(a.index.reads == firstReads + 15 && std::string(a.winItems[0].label) == "d");
  a.refreshRowWindow(3, 2); // orientation/row-height capacity changed
  assert(a.winItems.size() == 2 && a.index.reads == firstReads + 25);
  a.index.rows.pop_back(); // count change cannot leave a stale window
  int before = a.index.reads;
  a.refreshRowWindow(3, 2);
  assert(a.index.reads == before + 10);

  a.selectTab(TITLE_TAB, true); // reverse order, same viewport and count
  a.refreshRowWindow(0, 3);
  assert(std::string(a.winItems[0].label) == "g");
  a.query = "b";
  a.applyFilter();
  a.refreshRowWindow(0, 3);
  assert(a.winItems.size() == 1 && std::string(a.winItems[0].label) == "b");
  a.query = "c"; // same-size result must invalidate too
  a.applyFilter();
  a.refreshRowWindow(0, 3);
  assert(a.winItems.size() == 1 && std::string(a.winItems[0].label) == "c");
  a.query.clear();
  a.applyFilter();
  a.refreshRowWindow(0, 3);
  assert(std::string(a.winItems[0].label) == "g");

  a.groupStarts = makeUniqueNoThrow<uint16_t[]>(3);
  a.groupStarts[0] = 0; a.groupStarts[1] = 2; a.groupStarts[2] = 4;
  a.groupCount = 3;
  a.expandedNav.selected = 4; a.expandedNav.top = 3;
  a.groupsCollapsed = true;
  a.refreshRowWindow(0, 3);
  assert(std::string(a.winItems[0].label) == "G" && a.winItems[0].icon == 0);
  a.restoreExpandedList();
  a.refreshRowWindow(0, 3);
  assert(std::string(a.winItems[0].label) == "g" && a.winItems[0].icon == 1);
  assert(a.nav.selected == 4 && a.nav.top == 3);

  a.index.rows.back().title = "changed"; // published replacement has the SAME count
  a.resetAfterRebuild();
  a.refreshRowWindow(0, 3);
  assert(std::string(a.winItems[0].label) == "changed");
  assert(a.nav.selected == 4 && a.nav.top == 3);

  a.selectTab(RECENT_TAB, false);
  RECENT_BOOKS.books = {{"/a", "Pinned a", "Pinned author"}};
  a.resolvePinned();
  a.refreshRowWindow(0, 3);
  assert(std::string(a.winItems[0].label) == "Pinned a");
  RECENT_BOOKS.books = {{"/b", "Pinned b", "New author"}};
  a.resolvePinned(); // same pin count, different data
  a.refreshRowWindow(0, 3);
  assert(std::string(a.winItems[0].label) == "Pinned b");
  RECENT_BOOKS.books.clear();
  a.resolvePinned();
  a.refreshRowWindow(0, 3);
  assert(std::string(a.winItems[0].label) == "changed");

  a.index.failed = true;
  a.refreshOverlap();
  a.refreshRowWindow(0, 3);
  assert(a.winStart == -1); // a failed read cannot poison the cache
  a.index.failed = false;
  a.refreshRowWindow(0, 3);
  assert(std::string(a.winItems[0].label) == "changed");
  assert(benchmark::clocks == 0 && benchmark::spans == 0);
  benchmark::recording = true;
  a.refreshRowWindow(0, 3);
  assert(benchmark::spans == 1 && benchmark::lastScreen == 1 && benchmark::lastHit == 1 && benchmark::lastRows == 0);
  a.refreshRowWindow(3, 3);
  assert(benchmark::spans == 2 && benchmark::lastHit == 0 && benchmark::lastRows == 3);
  puts("Library row-window cache/invalidation behavior passed");
}
'''

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "library_row_window.cpp"
    executable = Path(directory) / "library_row_window"
    cpp.write_text(harness)
    subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "lib/Benchmark"),
        str(cpp), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
