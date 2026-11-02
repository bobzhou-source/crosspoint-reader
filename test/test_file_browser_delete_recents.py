"""Exercise the production File Browser delete against the recents list.

Run: python3 test/test_file_browser_delete_recents.py (requires a host C++ compiler).
Compiles FileBrowserActivity::removeDirFile and RecentBooksStore::removeByPath
verbatim against an in-memory card. No device, panel, SD-card image, or firmware
build is used.
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
browser_source = (ROOT / "src/activities/home/FileBrowserActivity.cpp").read_text()
recents_source = (ROOT / "src/RecentBooksStore.cpp").read_text()


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


name_buffer_line = next(line.strip() for line in browser_source.splitlines()
                        if "constexpr size_t NAME_BUFFER_SIZE" in line)

harness = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#define LOG_ERR(...) ((void)0)
''' + name_buffer_line + r'''

// In-memory card: regular files and directories keyed by absolute path.
struct Card {
  std::set<std::string> files, dirs, failRemove;
  std::vector<std::string> cleared;
  static std::string parent(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == 0 ? "/" : path.substr(0, slash);
  }
  std::vector<std::string> children(const std::string& dir) const {
    std::vector<std::string> out;
    for (const auto& f : files) if (parent(f) == dir) out.push_back(f);
    for (const auto& d : dirs) if (d != "/" && parent(d) == dir) out.push_back(d);
    std::sort(out.begin(), out.end());
    return out;
  }
} card;

struct FsFile {
  std::string path;
  bool valid = false;
  std::vector<std::string> entries;
  size_t next = 0;
  explicit operator bool() const { return valid; }
  bool isDirectory() const { return valid && card.dirs.count(path) != 0; }
  void close() {}
  void rewindDirectory() { entries = card.children(path); next = 0; }
  FsFile openNextFile() {
    if (next >= entries.size()) return FsFile{};
    FsFile f;
    f.path = entries[next++];
    f.valid = true;
    return f;
  }
  void getName(char* buffer, size_t size) const {
    const std::string name = path.substr(path.find_last_of('/') + 1);
    std::snprintf(buffer, size, "%s", name.c_str());
  }
};

struct StorageDouble {
  FsFile open(const char* path) {
    FsFile f;
    f.path = path;
    f.valid = card.files.count(path) || card.dirs.count(path);
    return f;
  }
  bool remove(const char* path) {
    if (card.failRemove.count(path) || !card.files.count(path)) return false;
    card.files.erase(path);
    return true;
  }
  bool rmdir(const char* path) {
    if (!card.dirs.count(path) || !card.children(path).empty()) return false;
    card.dirs.erase(path);
    return true;
  }
} Storage;

void clearBookCache(const std::string& path) { card.cleared.push_back(path); }

struct RecentBook { std::string path, title, author, coverBmpPath; };
struct RecentBooksStore {
  std::vector<RecentBook> recentBooks;
  uint32_t revision = 0;
  int saves = 0;
  bool saveToFile() { ++saves; return true; }
  bool removeByPath(const std::string& path);
} RECENT_BOOKS;
''' + method(recents_source, "bool RecentBooksStore::removeByPath(") + r'''

struct FileBrowserActivity {
  std::unique_ptr<char[]> fileNameBuffer{new char[NAME_BUFFER_SIZE]};
  bool removeDirFile(const std::string& fullPath);
};
''' + method(browser_source, "bool FileBrowserActivity::removeDirFile(") + r'''

static void reset(std::set<std::string> files, std::set<std::string> dirs, std::vector<std::string> recents) {
  card = Card{};
  card.files = std::move(files);
  card.dirs = std::move(dirs);
  card.dirs.insert("/");
  RECENT_BOOKS = RecentBooksStore{};
  for (const auto& path : recents) RECENT_BOOKS.recentBooks.push_back({path, "t", "a", "/.crosspoint/c/thumb.bmp"});
}

static std::vector<std::string> recentPaths() {
  std::vector<std::string> out;
  for (const auto& book : RECENT_BOOKS.recentBooks) out.push_back(book.path);
  return out;
}

int main() {
  FileBrowserActivity browser;

  // Deleting a listed book removes exactly that entry, keeps the others in order, persists once.
  reset({"/Books/a.epub", "/Books/b.epub", "/x.txt"}, {"/Books"}, {"/Books/b.epub", "/Books/a.epub", "/x.txt"});
  assert(browser.removeDirFile("/Books/a.epub"));
  assert(!card.files.count("/Books/a.epub") && card.files.count("/Books/b.epub"));
  assert(card.cleared == std::vector<std::string>{"/Books/a.epub"});
  assert((recentPaths() == std::vector<std::string>{"/Books/b.epub", "/x.txt"}));
  assert(RECENT_BOOKS.saves == 1 && RECENT_BOOKS.revision == 1);

  // Deleting a book that is not listed leaves the recents list and its file untouched.
  reset({"/Books/a.epub", "/Books/b.epub"}, {"/Books"}, {"/Books/b.epub"});
  assert(browser.removeDirFile("/Books/a.epub"));
  assert((recentPaths() == std::vector<std::string>{"/Books/b.epub"}));
  assert(RECENT_BOOKS.saves == 0 && RECENT_BOOKS.revision == 0);

  // Deleting a folder removes every listed book inside it (nested too), not a sibling
  // folder sharing the name prefix.
  reset({"/Books/a.epub", "/Books/sub/c.epub", "/Books/notes.txt", "/Bookshelf/d.epub", "/e.epub"},
        {"/Books", "/Books/sub", "/Bookshelf"},
        {"/Bookshelf/d.epub", "/Books/sub/c.epub", "/e.epub", "/Books/a.epub"});
  assert(browser.removeDirFile("/Books"));
  assert(!card.dirs.count("/Books") && !card.dirs.count("/Books/sub") && card.dirs.count("/Bookshelf"));
  assert((recentPaths() == std::vector<std::string>{"/Bookshelf/d.epub", "/e.epub"}));
  assert(RECENT_BOOKS.saves == 2);

  // A failed delete keeps the book listed: the file is still on the card.
  reset({"/Books/a.epub"}, {"/Books"}, {"/Books/a.epub"});
  card.failRemove.insert("/Books/a.epub");
  assert(!browser.removeDirFile("/Books/a.epub"));
  assert((recentPaths() == std::vector<std::string>{"/Books/a.epub"}));
  assert(RECENT_BOOKS.saves == 0);

  // A folder delete that stops part-way unlists only the books actually removed.
  reset({"/Books/a.epub", "/Books/b.epub"}, {"/Books"}, {"/Books/a.epub", "/Books/b.epub"});
  card.failRemove.insert("/Books/b.epub");
  assert(!browser.removeDirFile("/Books"));
  assert(!card.files.count("/Books/a.epub") && card.files.count("/Books/b.epub") && card.dirs.count("/Books"));
  assert((recentPaths() == std::vector<std::string>{"/Books/b.epub"}));
  assert(RECENT_BOOKS.saves == 1);

  std::puts("File Browser delete: removed books leave recents; unlisted, failed and partial deletes keep the rest");
}
'''

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "file_browser_delete_recents.cpp"
    executable = Path(directory) / "file_browser_delete_recents"
    cpp.write_text(harness)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=30)
