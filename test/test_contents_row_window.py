"""Run production EPUB/TXT Contents window loaders with counted row reads.

Run: python3 test/test_contents_row_window.py (requires a host C++ compiler).
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(filename, signature):
    source = (ROOT / "src/activities/reader" / filename).read_text()
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


def constant(filename, name):
    source = (ROOT / "src/activities/reader" / filename).read_text()
    start = source.index("static constexpr int " + name)
    return source[start:source.index(";", start) + 1]


epub_method = method("EpubReaderChapterSelectionActivity.cpp", "void EpubReaderChapterSelectionActivity::refreshTocWindow(")
txt_method = method("TxtReaderChapterSelectionActivity.cpp", "void TxtReaderChapterSelectionActivity::refreshRowWindow(")

harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
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
#define tr(key) #key
namespace fui {
struct ListItem { const char* label = nullptr; const char* value = nullptr; int16_t actionValue = 0; };
}
struct Scale { int bodyFontId = 0; };
Scale uiScaleSpec() { return {}; }
struct Renderer {
  int batches = 0, labels = 0;
  void prewarmFallbackText(int, const char* (*getter)(const void*, uint32_t), const void* ctx, uint32_t count) {
    ++batches;
    for (uint32_t i = 0; i < count; ++i) { assert(getter(ctx, i)); ++labels; }
    assert(getter(ctx, count) == nullptr);
  }
};
struct Nav { int visibleRows = 6, selected = 0, top = 0; };
struct Epub {
  struct Item { int level; std::string title; };
  int count = 6932, reads = 0;
  Item getTocItem(int index) {
    assert(index >= 0 && index < count); ++reads;
    return {index % 3 + 1, "Chapter " + std::to_string(index)};
  }
};
struct EpubReaderChapterSelectionActivity {
''' + constant("EpubReaderChapterSelectionActivity.h", "TOC_WINDOW") + r'''
  Epub book;
  Epub* epub = &book;
  Nav nav;
  Renderer renderer;
  int currentTocIndex = 7, windowStart = -1, windowCount = 0;
  std::string windowLabels[TOC_WINDOW];
  fui::ListItem windowItems[TOC_WINDOW];
  int listCount() const { return book.count; }
  void refreshTocWindow(int);
};
struct PageIndex {
  std::vector<size_t> offsets;
  const auto& chapters() const { return offsets; }
  size_t pageContaining(size_t offset) const { return offset / 100; }
};
struct TxtReaderChapterSelectionActivity {
''' + constant("TxtReaderChapterSelectionActivity.h", "ROW_WINDOW") + r'''
  PageIndex pageIndex;
  Nav nav;
  Renderer renderer;
  int currentChapter = 7, windowStart = -1, windowCount = 0, reads = 0;
  std::string windowLabels[ROW_WINDOW];
  char windowValues[ROW_WINDOW][12]{};
  fui::ListItem windowItems[ROW_WINDOW];
  int listCount() const { return pageIndex.offsets.size(); }
  bool readTitle(size_t offset, std::string& title) { ++reads; title = "Chapter " + std::to_string(offset / 100); return true; }
  void refreshRowWindow(int);
};
''' + epub_method + "\n" + txt_method + r'''
int main() {
  EpubReaderChapterSelectionActivity epub;
  TxtReaderChapterSelectionActivity txt;
  for (int i = 0; i < epub.listCount(); ++i) txt.pageIndex.offsets.push_back(i * 100);
  const auto refresh = [&](int top) { epub.refreshTocWindow(top); txt.refreshRowWindow(top); };
  refresh(0);
  const int window = epub.TOC_WINDOW;
  assert(window == txt.ROW_WINDOW && epub.book.reads == window && txt.reads == window);
  assert(std::string(epub.windowItems[7].value) == "STR_CURRENT_CHAPTER");
  assert(std::string(txt.windowItems[7].value) == "STR_CURRENT_CHAPTER");
  assert(std::string(txt.windowItems[8].value) == "9");
  // Three page moves + back within the existing 24-row window need neither SD
  // titles/LUT reads nor another glyph prewarm. Selection belongs to ListNav.
  for (int top : {6, 12, 18, 12, 0}) {
    epub.nav.selected = txt.nav.selected = 7;
    refresh(top);
    assert(epub.windowStart == 0 && txt.windowStart == 0);
    assert(epub.book.reads == window && txt.reads == window);
    assert(epub.nav.selected == 7 && txt.nav.selected == 7);
  }
  assert(epub.renderer.batches == 1 && txt.renderer.batches == 1);
  printf("Contents: cold=%d row reads, five in-window moves=0, prewarm batches=1\n", window);
  refresh(19); // viewport would need row 24: replenish rather than leaving a gap
  assert(epub.windowStart == 19 && txt.windowStart == 19);
  assert(epub.windowItems[0].actionValue == 19 && txt.windowItems[0].actionValue == 19);
  assert(epub.book.reads == 2 * window && txt.reads == 2 * window);
  refresh(18); // backwards outside the cached window
  assert(epub.windowStart == 18 && txt.windowStart == 18);

  // A larger viewport must not reuse a window that no longer covers it.
  epub.nav.visibleRows = txt.nav.visibleRows = 12;
  refresh(36);
  assert(epub.windowStart == 36 && txt.windowStart == 36);
  refresh(6930);
  assert(epub.windowStart == 6932 - window && txt.windowStart == 6932 - window);
  assert(epub.windowItems[window - 1].actionValue == 6931);
  assert(txt.windowItems[window - 1].actionValue == 6931);
  const int tailReads = epub.book.reads;
  refresh(6931);
  refresh(6926);
  assert(epub.book.reads == tailReads && txt.reads == tailReads);

  EpubReaderChapterSelectionActivity shortEpub;
  shortEpub.book.count = 3;
  shortEpub.refreshTocWindow(0);
  shortEpub.refreshTocWindow(2);
  assert(shortEpub.windowStart == 0 && shortEpub.windowCount == 3 && shortEpub.book.reads == 3);
  TxtReaderChapterSelectionActivity shortTxt;
  shortTxt.pageIndex.offsets = {0, 100, 200};
  shortTxt.refreshRowWindow(0);
  shortTxt.refreshRowWindow(2);
  assert(shortTxt.windowStart == 0 && shortTxt.windowCount == 3 && shortTxt.reads == 3);
  assert(benchmark::clocks == 0 && benchmark::spans == 0);
  benchmark::recording = true;
  refresh(6926);
  assert(benchmark::spans == 2 && benchmark::lastScreen == 2 && benchmark::lastHit == 1 && benchmark::lastRows == 0);
  refresh(0);
  assert(benchmark::spans == 4 && benchmark::lastHit == 0 && benchmark::lastRows == static_cast<uint32_t>(window));
  puts("EPUB/TXT Contents window behavior passed");
}
'''

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "contents_row_window.cpp"
    executable = Path(directory) / "contents_row_window"
    cpp.write_text(harness)
    subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "lib/Benchmark"),
        str(cpp), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
