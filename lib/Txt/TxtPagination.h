#pragma once

#include <HalStorage.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Plain-text page layout shared by the page index and page rendering. Each page
// is laid out from a bounded source window starting at its offset, so a page's
// lines are a function of its start offset alone: the index and the reader
// always agree on where the next page starts.
namespace TxtPagination {

// Every page is laid out from at most this many source bytes starting at its offset.
static constexpr size_t PAGE_INPUT_BYTES = 8 * 1024;

// Source bytes a page starting at `offset` is laid out from.
constexpr size_t pageInputLength(const size_t offset, const size_t fileSize) {
  return offset < fileSize ? std::min(PAGE_INPUT_BYTES, fileSize - offset) : 0;
}

enum class ReadStatus : uint8_t {
  Ok,
  InvalidRange,
  AllocationFailed,
  OpenFailed,
  SeekFailed,
  ReadError,  // HAL reported an error (negative count)
  ShortRead,  // source ended before the requested range was filled
  Interrupted,  // cooperative background layout yielded; retry this page
};

struct TextMeasure {
  void* ctx;
  // Optional. Called once per loaded page with the NUL-terminated page input, before any width query.
  void (*preparePage)(void* ctx, const char* pageText);
  // Pixel advance of a NUL-terminated UTF-8 string, exactly as the line is drawn.
  int (*advanceX)(void* ctx, const char* text);
  // Optional. Advance of one code point, used only to estimate where a line
  // ends; every chosen line is confirmed with advanceX. Without it the
  // estimate measures each code point through advanceX.
  int (*codepointAdvance)(void* ctx, uint32_t cp) = nullptr;
  // Optional background-work interrupt. Checked around preparation and between lines.
  // Uses ctx; never called by a layout whose callback is null.
  bool (*shouldYield)(void* ctx) = nullptr;
};

struct PageLayout {
  int viewportWidth;
  int linesPerPage;
  TextMeasure measure;
  // CJK paragraph style: whitespace is cleaned (a run of spaces, tabs and
  // other controls shows as one space; BOM and zero-width spaces vanish;
  // leading whitespace of every line and trailing whitespace of a complete
  // line is dropped), blank lines produce no display line, and the first line
  // of a paragraph is indented by `firstLineIndent` pixels. Layout only; byte
  // offsets still address the unmodified source.
  bool cjkParagraphs = false;
  int firstLineIndent = 0;
};

struct LineSink {
  void* ctx;
  // One display line; `text` is not NUL-terminated and is only valid during the
  // call. `indent` is the pixel inset of a paragraph's first line.
  void (*line)(void* ctx, const char* text, size_t length, int indent);
};

struct ParagraphSink {
  void* ctx;
  // A complete physical line that starts on the page: its source offset and
  // bytes without the line terminator. Called once per line across a page
  // sweep; a line cut by the page input edge is not reported (it is at least
  // 1KB long, longer than any heading).
  void (*paragraph)(void* ctx, size_t offset, const char* text, size_t length);
};

// Lays out the page whose input is `text[0, length)` (source offset `pageOffset`).
// `startsLine` says whether the page starts a physical line (offset 0 or right
// after '\n'). `text` must be writable with text[length] == '\0': width queries
// NUL-terminate candidate spans in place and restore every byte before the next
// query. `sink` and `paragraphs` may be null. Returns the number of display
// lines; `nextOffset` receives the start of the following page. On interruption
// returns -1 with nextOffset == pageOffset; callers discard emitted lines/headings.
//
// Lines break at spaces, between CJK characters (with kinsoku: closing
// punctuation never starts a line, opening brackets never end one) and, for an
// unbreakable run wider than the line, at the last whole code point that fits.
// A display line the window edge cuts before its break is known is never
// broken at the edge: it starts the next page, or for a page holding nothing
// else, is forced up to the last complete code point in the window. A physical
// line cut by the edge with less than 1KB in the window moves to the next page whole.
int layoutPage(char* text, size_t length, size_t pageOffset, size_t fileSize, bool startsLine,
               const PageLayout& layout, const LineSink* sink, const ParagraphSink* paragraphs, size_t& nextOffset);

// One retained source handle plus one contiguous window of at most
// WINDOW_BYTES. A load that starts inside the current window keeps the
// overlapping bytes and reads only the newly exposed suffix; any other load
// seeks and refills. Bytes returned always equal a fresh read of that range.
class SourceWindow {
 public:
  // A page input plus the byte before it, which says whether the page starts a line.
  static constexpr size_t WINDOW_BYTES = PAGE_INPUT_BYTES + 1;

  SourceWindow() = default;
  ~SourceWindow() { release(); }
  SourceWindow(const SourceWindow&) = delete;
  SourceWindow& operator=(const SourceWindow&) = delete;

  // Binds the window to `path`, releasing any previous source and window.
  void setSource(std::string path);
  // Makes source bytes [offset, offset + length) available as writable `text`
  // with text[length] == '\0'. On failure `text` is null, no partially read
  // bytes are retained and the handle is closed so the next load reopens it.
  ReadStatus load(size_t offset, size_t length, char*& text);
  // Closes the source and frees the window buffer; setSource/load may be used again.
  void release();

 private:
  void invalidate();
  ReadStatus fill(size_t from, char* dst, size_t count);

  std::string path;
  HalFile file;
  std::unique_ptr<char[]> buffer;  // WINDOW_BYTES + 1 terminator slot
  size_t windowStart = 0;
  size_t windowLength = 0;  // valid bytes at buffer[0, windowLength); 0 = no reusable window
  size_t filePosition = 0;  // meaningful only while `file` is open
};

struct PageResult {
  ReadStatus status;
  int lineCount;
  size_t nextOffset;  // == offset unless status is Ok
};

// Loads the input of the page at `offset` through `window` and lays it out.
// An offset at or past the end of the source is InvalidRange.
PageResult loadPage(SourceWindow& window, size_t offset, size_t fileSize, const PageLayout& layout,
                    const LineSink* sink, const ParagraphSink* paragraphs = nullptr);

enum class IndexStatus : uint8_t {
  Ok,
  ReadFailed,  // a page could not be read; see IndexResult::read
  NoProgress,  // a page did not advance, so the sweep could never reach the end
  Interrupted,  // the frontier page was abandoned; earlier completed pages remain
};

struct IndexResult {
  IndexStatus status;
  ReadStatus read;  // ReadFailed/Interrupted page status; otherwise Ok
  size_t offset;    // frontier on return, or the source end when complete
};

// Chapter headings recorded per index; further headings are ignored.
static constexpr size_t MAX_CHAPTERS = 8192;

// Index of the page containing `offset` in a page-start list (offsets[0] == 0).
inline size_t pageContaining(const std::vector<uint32_t>& offsets, const size_t offset) {
  const auto it = std::upper_bound(offsets.begin(), offsets.end(), offset);
  return it == offsets.begin() ? 0 : static_cast<size_t>(it - offsets.begin()) - 1;
}

// Sweeps the whole source page by page through `window`, filling `offsets` with the start offset of every page
// (offsets[0] == 0; a source with no bytes has the single page 0) and `chapters` (optional) with the source offset
// of every chapter heading line. Only a completed sweep leaves offsets behind: on any failure `offsets` is empty,
// so a partial list can never be mistaken for a page index. `pageDone` (optional) runs after each page with the
// number of offsets recorded so far.
IndexResult buildPageIndex(SourceWindow& window, size_t fileSize, const PageLayout& layout,
                           std::vector<uint32_t>& offsets, std::vector<uint32_t>* chapters = nullptr,
                           void (*pageDone)(void* ctx, size_t pageCount) = nullptr, void* ctx = nullptr);

// The page index built a few pages at a time, so a reader can show pages while
// the rest of the book is indexed. offsets() always holds every page start
// found so far; the last one is the frontier page, laid out by the next step.
// Stepping to completion yields exactly the offsets and chapters of
// buildPageIndex.
class PageIndexBuilder {
 public:
  // Starts an empty index of a source of `fileSize` bytes.
  void reset(size_t fileSize);
  // Continues from a persisted index (validated by the caller): `offsets` are
  // the page starts found so far, `chapters` the headings on every page before
  // the frontier.
  void adopt(size_t fileSize, std::vector<uint32_t>&& offsets, std::vector<uint32_t>&& chapters, bool complete);
  // Lays out at most `maxPages` frontier pages. A failed step keeps the pages
  // indexed before the failure; interruption discards only the unfinished page and its headings.
  IndexResult step(SourceWindow& window, const PageLayout& layout, size_t maxPages);

  [[nodiscard]] bool complete() const { return done; }
  [[nodiscard]] size_t sourceSize() const { return fileSize; }
  [[nodiscard]] const std::vector<uint32_t>& offsets() const { return pageOffsets; }
  [[nodiscard]] const std::vector<uint32_t>& chapters() const { return chapterOffsets; }
  // Start of the frontier page: every byte before it lies on a known page.
  [[nodiscard]] size_t frontier() const { return done ? fileSize : pageOffsets.back(); }
  // Whether the page containing `offset` is known.
  [[nodiscard]] bool covers(const size_t offset) const { return done || offset <= pageOffsets.back(); }
  [[nodiscard]] size_t pageContaining(const size_t offset) const {
    return TxtPagination::pageContaining(pageOffsets, offset);
  }

 private:
  std::vector<uint32_t> pageOffsets{0};
  std::vector<uint32_t> chapterOffsets;
  size_t fileSize = 0;
  bool done = false;
};

}  // namespace TxtPagination
