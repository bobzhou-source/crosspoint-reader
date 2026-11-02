#include "TxtPagination.h"

#include <Memory.h>

#include <cstring>
#include <limits>
#include <utility>

#include "TxtChapters.h"
#include "TxtLineBreak.h"

namespace TxtPagination {

namespace {

using TxtLineBreak::decodeAt;

constexpr size_t NO_BREAK = std::numeric_limits<size_t>::max();

bool yieldRequested(const TextMeasure& measure) {
  return measure.shouldYield && measure.shouldYield(measure.ctx);
}
// A line cut by the window edge with fewer bytes than this in the window moves
// to the next page whole rather than being split; longer lines (no heading is
// this long) are laid out up to the edge.
constexpr size_t SPLIT_LINE_BYTES = 1024;

// Width of text[start, start + spanLength). The span is NUL-terminated in place
// for the query and the displaced byte is restored immediately afterwards.
int measureSpan(char* text, const size_t length, const size_t start, const size_t spanLength,
                const TextMeasure& measure) {
  const size_t end = start + spanLength;
  if (end >= length) {
    return measure.advanceX(measure.ctx, text + start);  // text[length] is the terminator
  }
  const char displaced = text[end];
  text[end] = '\0';
  const int width = measure.advanceX(measure.ctx, text + start);
  text[end] = displaced;
  return width;
}

// A break opportunity before the code point `cp` that follows `prev` on the
// line: a space (the line ends before it and the space is dropped), or a
// CJK boundary.
bool isCandidate(const uint32_t prev, const uint32_t cp) {
  return cp == ' ' || (prev != ' ' && TxtLineBreak::canBreakBetween(prev, cp));
}

struct LineBreak {
  size_t end;        // the line is text[start, end)
  size_t next;       // start of the following line
  bool determined;   // false: the span ran out (window edge) before the line could be closed
  bool interrupted = false;
};

// Greedy line breaking for one paragraph span. Each line costs one forward
// scan over the code points that fit, summing per-code-point advances to find
// the last break opportunity that fits; that line is then confirmed (and, if
// the estimate was off, moved to the neighbouring opportunity) with the exact
// advance of the whole line.
class Breaker {
 public:
  Breaker(char* text, const size_t length, const TextMeasure& measure)
      : text(text), length(length), measure(measure) {}

  LineBreak breakLine(const size_t start, const size_t end, const bool complete, const int width) const {
    int estimate = 0;
    size_t p = start;
    uint32_t prev = 0;
    size_t best = NO_BREAK;
    size_t bestNext = 0;
    while (p < end && estimate <= width) {
      if (yieldRequested(measure)) return {start, start, false, true};
      uint32_t cp = 0;
      const size_t n = decodeAt(text, p, end, cp);
      if (p > start && isCandidate(prev, cp)) {
        best = p;
        bestNext = cp == ' ' ? p + 1 : p;
      }
      estimate += advance(cp, p, n);
      prev = cp;
      p += n;
    }
    if (p >= end && estimate <= width) {
      if (!complete) return {end, end, false};
      best = end;
      bestNext = end;
    }

    if (best != NO_BREAK) {
      if (yieldRequested(measure)) return {start, start, false, true};
      if (exact(start, best) <= width) {
        if (best == end) return {best, bestNext, true};
        // The estimate may undershoot what fits: take later opportunities while they still fit.
        size_t scan = p;
        uint32_t scanPrev = prev;
        size_t candidate = 0;
        size_t candidateNext = 0;
        while (nextCandidate(start, end, complete, scan, scanPrev, candidate, candidateNext)) {
          if (yieldRequested(measure)) return {start, start, false, true};
          if (exact(start, candidate) > width) break;
          best = candidate;
          bestNext = candidateNext;
        }
        return {best, bestNext, true};
      }
      // The estimate overshot: step back to the last opportunity that really fits.
      while (previousCandidate(start, end, complete, best, best, bestNext)) {
        if (yieldRequested(measure)) return {start, start, false, true};
        if (exact(start, best) <= width) return {best, bestNext, true};
      }
    }
    return forcedBreak(start, end, complete, width);
  }

 private:
  int advance(const uint32_t cp, const size_t pos, const size_t n) const {
    if (measure.codepointAdvance && cp != TxtLineBreak::REPLACEMENT) {
      return measure.codepointAdvance(measure.ctx, cp);
    }
    return measureSpan(text, length, pos, n, measure);
  }

  int exact(const size_t start, const size_t end) const { return measureSpan(text, length, start, end - start, measure); }

  // Continues a forward scan at `p` (after code point `prev`) to the next break
  // opportunity, including the end of a complete span.
  bool nextCandidate(const size_t start, const size_t end, const bool complete, size_t& p, uint32_t& prev,
                     size_t& candidate, size_t& candidateNext) const {
    while (p < end) {
      uint32_t cp = 0;
      const size_t n = decodeAt(text, p, end, cp);
      const size_t at = p;
      const bool found = at > start && isCandidate(prev, cp);
      prev = cp;
      p += n;
      if (found) {
        candidate = at;
        candidateNext = cp == ' ' ? at + 1 : at;
        return true;
      }
    }
    if (complete && p == end) {
      p = end + 1;  // the span end is reported once
      candidate = end;
      candidateNext = end;
      return true;
    }
    return false;
  }

  // The last break opportunity before `before`, found by rescanning from `start`.
  bool previousCandidate(const size_t start, const size_t end, const bool complete, const size_t before,
                         size_t& candidate, size_t& candidateNext) const {
    size_t p = start;
    uint32_t prev = 0;
    size_t at = 0;
    size_t atNext = 0;
    bool found = false;
    size_t c = 0;
    size_t cNext = 0;
    while (nextCandidate(start, end, complete, p, prev, c, cNext) && c < before) {
      at = c;
      atNext = cNext;
      found = true;
    }
    if (found) {
      candidate = at;
      candidateNext = atNext;
    }
    return found;
  }

  // End of the grapheme-like cluster starting at `p`: a code point plus any combining marks.
  size_t clusterEnd(size_t p, const size_t end) const {
    uint32_t cp = 0;
    p += decodeAt(text, p, end, cp);
    while (p < end) {
      const size_t n = decodeAt(text, p, end, cp);
      if (!TxtLineBreak::isClusterExtender(cp)) break;
      p += n;
    }
    return p;
  }

  // No opportunity fits: break after the last whole cluster that fits, and
  // after at least one cluster so every line makes progress.
  LineBreak forcedBreak(const size_t start, const size_t end, const bool complete, const int width) const {
    const size_t first = clusterEnd(start, end);
    size_t best = first;
    size_t p = first;
    while (p < end) {
      if (yieldRequested(measure)) return {start, start, false, true};
      const size_t q = clusterEnd(p, end);
      if (q >= end && !complete) break;  // more marks may follow past the window
      if (exact(start, q) > width) break;
      best = q;
      p = q;
    }
    return {best, best, true};
  }

  char* text;
  size_t length;
  const TextMeasure& measure;
};

// ---- CJK paragraph style: cleaned view of a physical line ----

bool isClutter(const uint32_t cp) { return TxtLineBreak::isSpaceLike(cp) || TxtLineBreak::isInvisible(cp); }

// Whether cleanLine would change text[begin, end). A false positive only costs a copy.
bool needsClean(const char* text, const size_t begin, const size_t end, const bool complete) {
  size_t p = begin;
  bool prevSpace = false;
  bool first = true;
  while (p < end) {
    uint32_t cp = 0;
    const size_t n = decodeAt(text, p, end, cp);
    if (TxtLineBreak::isInvisible(cp)) return true;
    const bool space = TxtLineBreak::isSpaceLike(cp);
    if (space) {
      if (first) return true;
      if (cp != ' ' && cp != 0x00A0 && cp != 0x3000) return true;
      if (prevSpace) return true;
    }
    first = false;
    prevSpace = space;
    p += n;
  }
  return prevSpace && complete;
}

// `end` of text[begin, end) without its trailing whitespace and invisible characters.
size_t trimTrailing(const char* text, const size_t begin, size_t end) {
  while (end > begin) {
    size_t q = end - 1;
    while (q > begin && (static_cast<uint8_t>(text[q]) & 0xC0) == 0x80) q--;
    uint32_t cp = 0;
    if (decodeAt(text, q, end, cp) != end - q || !isClutter(cp)) break;
    end = q;
  }
  return end;
}

// Display copy of one CJK line. A cleaned line is never longer than its source (a run becomes one character no
// longer than the run, an invisible character vanishes), so one allocation of the line's length serves it, and the
// scratch is only ever allocated for a line that needs cleaning: at most PAGE_INPUT_BYTES + 1 text bytes and
// 2 * (PAGE_INPUT_BYTES + 1) offset bytes (24 KiB) per page, nothing for clean text. When the allocation fails
// the line is simply shown as it is.
static_assert(PAGE_INPUT_BYTES < 0x10000, "source offsets are 16-bit");
struct CleanScratch {
  std::unique_ptr<char[]> text;
  std::unique_ptr<uint16_t[]> source;  // source[i]: offset from the line start of the code point behind text[i]
  size_t capacity = 0;
  size_t size = 0;

  bool reserve(const size_t length) {
    if (capacity > length) return true;
    auto newText = makeUniqueNoThrow<char[]>(length + 1);
    auto newSource = makeUniqueNoThrow<uint16_t[]>(length + 1);
    if (!newText || !newSource) return false;
    text = std::move(newText);
    source = std::move(newSource);
    capacity = length + 1;
    return true;
  }
};

// The line text[begin, end) as it is displayed, in `s` (NUL-terminated, source[size] = the line length). Rules:
//  - invisible characters (BOM, zero-width space) are dropped;
//  - a run of whitespace becomes one character: a plain space, or the run's first NBSP/ideographic space when
//    it holds nothing else;
//  - whitespace leading the line and whitespace ending a complete line is dropped.
void cleanLine(const char* text, const size_t begin, const size_t end, const bool complete, CleanScratch& s) {
  s.size = 0;
  const auto put = [&](const char* bytes, const size_t count, const size_t at) {
    for (size_t i = 0; i < count; i++) {
      s.text[s.size] = bytes[i];
      s.source[s.size++] = static_cast<uint16_t>(at - begin);
    }
  };
  size_t p = begin;
  bool leading = true;
  while (p < end) {
    uint32_t cp = 0;
    const size_t n = decodeAt(text, p, end, cp);
    if (!isClutter(cp)) {
      put(text + p, n, p);
      leading = false;
      p += n;
      continue;
    }
    size_t q = p;
    bool plain = false;
    size_t keepAt = 0;
    size_t keepLength = 0;
    while (q < end) {
      uint32_t c = 0;
      const size_t m = decodeAt(text, q, end, c);
      if (!isClutter(c)) break;
      if (TxtLineBreak::isAsciiSpace(c)) {
        plain = true;
      } else if (TxtLineBreak::isSpaceLike(c) && keepLength == 0) {
        keepAt = q;
        keepLength = m;
      }
      q += m;
    }
    if (plain || keepLength > 0) {
      const bool atEnd = q >= end;
      bool drop = leading || (atEnd && complete);
      if (!drop) {
        if (plain) {
          put(" ", 1, p);
        } else {
          put(text + keepAt, keepLength, keepAt);
        }
      }
      leading = false;
    }
    p = q;
  }
  s.source[s.size] = static_cast<uint16_t>(end - begin);
  s.text[s.size] = '\0';
}

}  // namespace

int layoutPage(char* text, const size_t length, const size_t pageOffset, const size_t fileSize, const bool startsLine,
               const PageLayout& layout, const LineSink* sink, const ParagraphSink* paragraphs, size_t& nextOffset) {
  const bool atEof = pageOffset + length >= fileSize;
  // Bytes of a code point the window cut in half belong to the next page.
  const size_t decodable = atEof ? length : TxtLineBreak::completePrefix(text, length);
  CleanScratch scratch;

  int lineCount = 0;
  size_t pos = 0;
  bool paragraphStart = startsLine;
  const auto interrupted = [&]() {
    nextOffset = pageOffset;
    return -1;
  };

  const auto emit = [&](const char* buffer, const size_t start, const size_t spanLength, const int indent) {
    if (sink) sink->line(sink->ctx, buffer + start, spanLength, indent);
    lineCount++;
  };

  while (pos < length && lineCount < layout.linesPerPage) {
    if (yieldRequested(layout.measure)) return interrupted();
    const void* newline = std::memchr(text + pos, '\n', length - pos);
    const size_t lineEnd = newline ? static_cast<size_t>(static_cast<const char*>(newline) - text) : length;

    // A line cut by the end of the page input is laid out as far as the window
    // determines its line breaks. A short one moves to the next page whole, so
    // every line short enough to be a heading is seen complete by some page.
    const bool complete = newline != nullptr || atEof;
    if (!complete && pos > 0 && length - pos < SPLIT_LINE_BYTES) {
      break;
    }

    size_t contentEnd = complete ? lineEnd : std::max(pos, decodable);
    if (contentEnd > pos && text[contentEnd - 1] == '\r') {
      contentEnd--;
    }
    if (paragraphs && paragraphStart && complete) {
      paragraphs->paragraph(paragraphs->ctx, pageOffset + pos, text + pos, contentEnd - pos);
    }

    // The line laid out below: the source bytes, or (CJK paragraph style) a
    // cleaned copy of them whose indices sourceAt maps back to source offsets.
    char* buf = text;
    size_t bufLength = length;
    size_t begin = pos;  // the line as displayed starts here: CJK style drops leading whitespace
    size_t end = contentEnd;
    if (layout.cjkParagraphs) {
      while (begin < end) {
        uint32_t cp = 0;
        const size_t n = decodeAt(text, begin, end, cp);
        if (!isClutter(cp)) break;
        begin += n;
      }
      if (complete) end = trimTrailing(text, begin, end);
    }
    size_t cur = begin;  // start of the unconsumed remainder of this physical line
    size_t limit = end;
    const bool cleaned = layout.cjkParagraphs && needsClean(text, begin, end, complete) && scratch.reserve(end - begin);
    if (cleaned) {
      cleanLine(text, begin, end, complete, scratch);
      buf = scratch.text.get();
      bufLength = scratch.size;
      cur = 0;
      limit = bufLength;
    }
    const auto sourceAt = [&](const size_t index) { return cleaned ? begin + scratch.source[index] : index; };
    const Breaker breaker(buf, bufLength, layout.measure);

    if (layout.cjkParagraphs && paragraphStart && cur == limit) {
      // A blank line separates nothing the indent does not already mark: it takes no display line.
      if (!complete) {
        pos = sourceAt(cur);
        break;
      }
      pos = lineEnd + 1;
      continue;
    }

    if (cur == limit) {
      // An empty physical line is one blank display line; the empty rest of a
      // line the previous page ended at its window edge is nothing.
      if (paragraphStart) emit(buf, cur, 0, 0);
      if (!complete) {
        pos = sourceAt(cur);
        break;
      }
      pos = lineEnd + 1;
      paragraphStart = true;
      continue;
    }

    bool firstLine = paragraphStart;
    bool deferred = false;
    while (cur < limit && lineCount < layout.linesPerPage) {
      if (yieldRequested(layout.measure)) return interrupted();
      const int indent = layout.cjkParagraphs && firstLine ? layout.firstLineIndent : 0;
      LineBreak line = breaker.breakLine(cur, limit, complete, layout.viewportWidth - indent);
      if (line.interrupted || yieldRequested(layout.measure)) return interrupted();
      if (!line.determined) {
        // The window ends before this line does: the next page lays it out
        // from its own window, unless nothing else is on this page.
        if (lineCount > 0) {
          deferred = true;
          break;
        }
        line = {limit, limit, true};
      }
      emit(buf, cur, line.end - cur, indent);
      firstLine = false;
      cur = line.next;
      // A continued line never starts with whitespace: the break that fell before a run of NBSP/ideographic
      // spaces (the only whitespace a cleaned line keeps) leaves it at the start of the next line, so drop it here.
      while (layout.cjkParagraphs && cur < limit) {
        uint32_t cp = 0;
        const size_t n = decodeAt(buf, cur, limit, cp);
        if (!TxtLineBreak::isSpaceLike(cp)) break;
        cur += n;
      }
    }

    if (!deferred && cur >= limit && complete) {
      pos = lineEnd + 1;
      paragraphStart = true;
    } else {
      pos = sourceAt(cur);
      break;
    }
  }

  nextOffset = std::min(pageOffset + pos, fileSize);
  return lineCount;
}

void SourceWindow::setSource(std::string newPath) {
  release();
  path = std::move(newPath);
}

void SourceWindow::invalidate() {
  if (file.isOpen()) {
    file.close();
  }
  windowStart = 0;
  windowLength = 0;
  filePosition = 0;
}

void SourceWindow::release() {
  invalidate();
  buffer.reset();
}

ReadStatus SourceWindow::fill(const size_t from, char* dst, size_t count) {
  if (!file.isOpen()) {
    if (!Storage.openFileForRead("TXT", path, file)) {
      return ReadStatus::OpenFailed;
    }
    filePosition = 0;
  }
  if (filePosition != from) {
    if (!file.seek(from)) {
      return ReadStatus::SeekFailed;
    }
    filePosition = from;
  }
  while (count > 0) {
    const int got = file.read(dst, count);
    if (got < 0 || static_cast<size_t>(got) > count) {
      return ReadStatus::ReadError;
    }
    if (got == 0) {
      return ReadStatus::ShortRead;
    }
    dst += got;
    count -= static_cast<size_t>(got);
    filePosition += static_cast<size_t>(got);
  }
  return ReadStatus::Ok;
}

ReadStatus SourceWindow::load(const size_t offset, const size_t length, char*& text) {
  text = nullptr;
  if (length == 0 || length > WINDOW_BYTES || offset > std::numeric_limits<size_t>::max() - length) {
    return ReadStatus::InvalidRange;
  }
  if (!buffer) {
    buffer = makeUniqueNoThrow<char[]>(WINDOW_BYTES + 1);
    if (!buffer) {
      return ReadStatus::AllocationFailed;
    }
    windowLength = 0;
  }
  char* const window = buffer.get();

  // Keep the bytes already resident for the start of the requested range.
  size_t keep = 0;
  const size_t windowEnd = windowStart + windowLength;
  if (windowLength > 0 && offset >= windowStart && offset < windowEnd) {
    keep = std::min(windowEnd, offset + length) - offset;
    if (offset > windowStart) {
      std::memmove(window, window + (offset - windowStart), keep);
    }
  }
  windowLength = 0;  // nothing is reusable until the fill below completes

  if (keep < length) {
    const ReadStatus status = fill(offset + keep, window + keep, length - keep);
    if (status != ReadStatus::Ok) {
      invalidate();
      return status;
    }
  }

  window[length] = '\0';
  windowStart = offset;
  windowLength = length;
  text = window;
  return ReadStatus::Ok;
}

PageResult loadPage(SourceWindow& window, const size_t offset, const size_t fileSize, const PageLayout& layout,
                    const LineSink* sink, const ParagraphSink* paragraphs) {
  PageResult result{ReadStatus::InvalidRange, 0, offset};
  if (offset >= fileSize) {
    return result;
  }

  // The byte before the page says whether it starts a physical line.
  const size_t lead = offset > 0 ? 1 : 0;
  const size_t length = pageInputLength(offset, fileSize);
  char* text = nullptr;
  if (yieldRequested(layout.measure)) {
    result.status = ReadStatus::Interrupted;
    return result;
  }
  result.status = window.load(offset - lead, length + lead, text);
  if (result.status != ReadStatus::Ok) {
    return result;
  }
  const bool startsLine = lead == 0 || text[0] == '\n';
  text += lead;

  if (yieldRequested(layout.measure)) {
    result.status = ReadStatus::Interrupted;
    return result;
  }
  if (layout.measure.preparePage) {
    layout.measure.preparePage(layout.measure.ctx, text);
  }
  if (yieldRequested(layout.measure)) {
    result.status = ReadStatus::Interrupted;
    return result;
  }
  result.lineCount =
      layoutPage(text, length, offset, fileSize, startsLine, layout, sink, paragraphs, result.nextOffset);
  if (result.lineCount < 0) {
    result.status = ReadStatus::Interrupted;
    result.lineCount = 0;
  }
  return result;
}

namespace {

struct ChapterCollector {
  std::vector<uint32_t>* chapters;
};

void collectChapter(void* ctx, const size_t offset, const char* text, const size_t length) {
  auto* chapters = static_cast<ChapterCollector*>(ctx)->chapters;
  if (chapters->size() < MAX_CHAPTERS && TxtChapters::isHeading(text, length)) {
    chapters->push_back(static_cast<uint32_t>(offset));
  }
}

}  // namespace

IndexResult buildPageIndex(SourceWindow& window, const size_t fileSize, const PageLayout& layout,
                           std::vector<uint32_t>& offsets, std::vector<uint32_t>* chapters,
                           void (*pageDone)(void* ctx, size_t pageCount), void* ctx) {
  offsets.clear();
  offsets.push_back(0);  // the first page starts at offset 0
  if (chapters) chapters->clear();
  ChapterCollector collector{chapters};
  const ParagraphSink paragraphSink{&collector, collectChapter};

  size_t offset = 0;
  while (offset < fileSize) {
    const PageResult page = loadPage(window, offset, fileSize, layout, nullptr, chapters ? &paragraphSink : nullptr);
    if (page.status != ReadStatus::Ok) {
      offsets.clear();
      if (chapters) chapters->clear();
      return {page.status == ReadStatus::Interrupted ? IndexStatus::Interrupted : IndexStatus::ReadFailed,
              page.status, offset};
    }
    if (page.nextOffset <= offset) {
      offsets.clear();
      if (chapters) chapters->clear();
      return {IndexStatus::NoProgress, ReadStatus::Ok, offset};
    }

    offset = page.nextOffset;
    if (page.lineCount == 0 && offset >= fileSize) {
      // Only blank lines remain: the book ends on the previous page.
      if (offsets.size() > 1) offsets.pop_back();
    } else if (page.lineCount == 0 && offsets.size() > 1) {
      offsets.back() = static_cast<uint32_t>(offset);  // blank lines join the following page
    } else if (offset < fileSize) {
      offsets.push_back(static_cast<uint32_t>(offset));
    }
    if (pageDone) {
      pageDone(ctx, offsets.size());
    }
  }
  return {IndexStatus::Ok, ReadStatus::Ok, offset};
}

void PageIndexBuilder::reset(const size_t size) {
  pageOffsets.clear();
  pageOffsets.push_back(0);
  chapterOffsets.clear();
  fileSize = size;
  done = size == 0;
}

void PageIndexBuilder::adopt(const size_t size, std::vector<uint32_t>&& offsets, std::vector<uint32_t>&& chapters,
                             const bool complete) {
  pageOffsets = std::move(offsets);
  chapterOffsets = std::move(chapters);
  if (pageOffsets.empty()) pageOffsets.push_back(0);
  fileSize = size;
  done = complete || size == 0;
}

IndexResult PageIndexBuilder::step(SourceWindow& window, const PageLayout& layout, const size_t maxPages) {
  ChapterCollector collector{&chapterOffsets};
  const ParagraphSink paragraphSink{&collector, collectChapter};

  for (size_t laidOut = 0; laidOut < maxPages && !done; laidOut++) {
    const size_t offset = pageOffsets.back();
    const size_t chaptersBefore = chapterOffsets.size();
    const PageResult page = loadPage(window, offset, fileSize, layout, nullptr, &paragraphSink);
    if (page.status != ReadStatus::Ok) {
      chapterOffsets.resize(chaptersBefore);
      return {page.status == ReadStatus::Interrupted ? IndexStatus::Interrupted : IndexStatus::ReadFailed,
              page.status, offset};
    }
    if (page.nextOffset <= offset) {
      chapterOffsets.resize(chaptersBefore);
      return {IndexStatus::NoProgress, ReadStatus::Ok, offset};
    }

    const size_t next = page.nextOffset;
    if (page.lineCount == 0 && next >= fileSize) {
      // Only blank lines remain: the book ends on the previous page.
      if (pageOffsets.size() > 1) pageOffsets.pop_back();
      done = true;
    } else if (page.lineCount == 0 && pageOffsets.size() > 1) {
      pageOffsets.back() = static_cast<uint32_t>(next);  // blank lines join the following page
    } else if (next >= fileSize) {
      done = true;
    } else {
      pageOffsets.push_back(static_cast<uint32_t>(next));
    }
  }
  return {IndexStatus::Ok, ReadStatus::Ok, frontier()};
}

}  // namespace TxtPagination
