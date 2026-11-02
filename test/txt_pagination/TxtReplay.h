#pragma once

#include <TxtLineBreak.h>
#include <TxtPagination.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "LegacyReceiver.h"
#include "WidthOracle.h"

// Replays whole books through the production walker (and, for Latin text, the frozen legacy loader) on the
// same source bytes, width oracle and viewport, and reports where they differ. referenceLines() is the
// specification of line breaking the production walker must match.
namespace txt_replay {

namespace {
#include "NotesFixture.inc"
}  // namespace

static_assert(sizeof(NOTES_TXT_BYTES) == 165998, "benchmark Notes fixture size");

inline constexpr const char* kSourcePath = "/book.txt";

struct Config {
  int viewportWidth;
  int linesPerPage;
  WidthOracle::Model model;
  bool cjkParagraphs = false;
  int firstLineIndent = 0;
};

struct PageRecord {
  size_t start = 0;
  size_t next = 0;
  bool ok = false;
  TxtPagination::ReadStatus status = TxtPagination::ReadStatus::Ok;
  int lineCount = 0;
  std::vector<std::string> lines;  // empty in count-only mode
  std::vector<int> indents;        // first-line indent of each collected line
  std::vector<uint64_t> prepared;  // digest of each page text handed to the font preparation hook
};

struct Counters {
  int opens = 0;
  int seeks = 0;
  int reads = 0;
  int shortReads = 0;
  int errorReads = 0;
  size_t requestedBytes = 0;
  size_t actualBytes = 0;
  uint64_t widthCalls = 0;
  uint64_t widthBytes = 0;
  size_t maxWidthBytes = 0;
};

struct Sweep {
  std::vector<PageRecord> pages;
  bool completed = false;  // the sweep reached the end of the source
  Counters counters;
};

struct Faults {
  int errorOnReadCall = 0;
  int zeroOnReadCall = 0;
  size_t truncateAt = std::numeric_limits<size_t>::max();
};

inline std::string notesFixture() {
  return std::string(reinterpret_cast<const char*>(NOTES_TXT_BYTES), sizeof(NOTES_TXT_BYTES));
}

// Replaces the fake SD file, zeroing every counter and fault.
inline void installSource(const std::string& bytes, const Faults* faults = nullptr) {
  FakeSource& source = Storage.source();
  source = FakeSource{};
  source.data.assign(bytes.begin(), bytes.end());
  if (faults != nullptr) {
    source.errorOnReadCall = faults->errorOnReadCall;
    source.zeroOnReadCall = faults->zeroOnReadCall;
    source.truncateAt = faults->truncateAt;
  }
}

inline Counters snapshot(const WidthOracle& oracle) {
  const FakeSource& s = Storage.source();
  Counters c;
  c.opens = s.openCount;
  c.seeks = s.seekCount;
  c.reads = s.readCalls;
  c.shortReads = s.shortReads;
  c.errorReads = s.errorReads;
  c.requestedBytes = s.requestedBytes;
  c.actualBytes = s.actualBytes;
  c.widthCalls = oracle.calls;
  c.widthBytes = oracle.inputBytes;
  c.maxWidthBytes = oracle.maxInputBytes;
  return c;
}

// ---- production side ----

struct NewAdapter {
  const WidthOracle* oracle;
  std::vector<uint64_t>* prepared;
};

inline int newAdvance(void* ctx, const char* text) { return static_cast<NewAdapter*>(ctx)->oracle->width(text); }

inline void newPrepare(void* ctx, const char* pageText) {
  auto* prepared = static_cast<NewAdapter*>(ctx)->prepared;
  if (prepared != nullptr) prepared->push_back(textDigest(pageText));
}

inline void collectLine(void* ctx, const char* text, const size_t length, const int indent) {
  auto* page = static_cast<PageRecord*>(ctx);
  page->lines.emplace_back(text, length);
  page->indents.push_back(indent);
}

inline TxtPagination::PageLayout makeLayout(const Config& config, NewAdapter& adapter) {
  return TxtPagination::PageLayout{config.viewportWidth, config.linesPerPage,
                                   {&adapter, newPrepare, newAdvance, nullptr}, config.cjkParagraphs,
                                   config.firstLineIndent};
}

// Loads one page through the production walker. Count-only when `collectLines` is false.
inline PageRecord loadNewPage(TxtPagination::SourceWindow& window, const size_t fileSize, const Config& config,
                              WidthOracle& oracle, const size_t offset, const bool collectLines) {
  PageRecord page;
  page.start = offset;
  page.next = offset;
  NewAdapter adapter{&oracle, &page.prepared};
  const TxtPagination::PageLayout layout = makeLayout(config, adapter);
  const TxtPagination::LineSink sink{&page, collectLine};
  const TxtPagination::PageResult result =
      TxtPagination::loadPage(window, offset, fileSize, layout, collectLines ? &sink : nullptr);
  page.status = result.status;
  page.lineCount = result.lineCount;
  page.ok = result.status == TxtPagination::ReadStatus::Ok;
  page.next = result.nextOffset;
  return page;
}

// Sweeps the source page by page, each page starting where the previous one ended.
inline Sweep sweepNew(const std::string& bytes, const Config& config, const bool collectLines,
                      const Faults* faults = nullptr) {
  installSource(bytes, faults);
  WidthOracle oracle(config.model);
  TxtPagination::SourceWindow window;
  window.setSource(kSourcePath);

  Sweep sweep;
  size_t offset = 0;
  while (offset < bytes.size()) {
    PageRecord page = loadNewPage(window, bytes.size(), config, oracle, offset, collectLines);
    const bool advanced = page.ok && page.next > offset;
    offset = page.next;
    sweep.pages.push_back(std::move(page));
    if (!advanced) break;
  }
  sweep.completed = offset >= bytes.size();
  sweep.counters = snapshot(oracle);
  return sweep;
}

// Every display line of a sweep, in order.
inline std::vector<std::string> sweepLines(const Sweep& sweep) {
  std::vector<std::string> lines;
  for (const PageRecord& page : sweep.pages) lines.insert(lines.end(), page.lines.begin(), page.lines.end());
  return lines;
}

// ---- frozen legacy side ----

inline PageRecord loadLegacyPage(LegacyReceiver& receiver, FakeRenderer& renderer, const size_t offset) {
  PageRecord page;
  page.start = offset;
  page.next = offset;
  renderer.prepared.clear();
  page.ok = receiver.loadPageAtOffset(renderer, offset, page.lines, page.next);
  page.lineCount = static_cast<int>(page.lines.size());
  page.indents.assign(page.lines.size(), 0);
  page.prepared = renderer.prepared;
  return page;
}

inline std::unique_ptr<LegacyReceiver> makeLegacyReceiver(const size_t fileSize, const Config& config) {
  auto receiver = std::make_unique<LegacyReceiver>();
  receiver->txt = std::make_unique<LegacyTxt>(kSourcePath, fileSize);
  receiver->viewportWidth = config.viewportWidth;
  receiver->linesPerPage = config.linesPerPage;
  return receiver;
}

inline Sweep sweepLegacy(const std::string& bytes, const Config& config) {
  installSource(bytes);
  WidthOracle oracle(config.model);
  FakeRenderer renderer(oracle, /*sdFont=*/true);
  const std::unique_ptr<LegacyReceiver> receiver = makeLegacyReceiver(bytes.size(), config);

  Sweep sweep;
  size_t offset = 0;
  while (offset < bytes.size()) {
    PageRecord page = loadLegacyPage(*receiver, renderer, offset);
    const bool advanced = page.ok && page.next > offset;
    offset = page.next;
    sweep.pages.push_back(std::move(page));
    if (!advanced) break;
  }
  sweep.completed = offset >= bytes.size();
  sweep.counters = snapshot(oracle);
  return sweep;
}

// ---- reference line breaking ----

// The line-breaking specification, written for clarity over speed. Every physical line is broken greedily at
// the last break opportunity whose whole prefix fits: before a space (which is dropped) or between characters
// where kinsoku allows a CJK break. With no such opportunity the line ends after the last whole cluster that
// fits, and after at least one. Widths must not decrease as a prefix grows (the Simple oracle on valid UTF-8),
// so the scan stops at the first prefix that is too wide. Assumes the whole source is in view: it is what a
// page sweep must produce whenever no display line reaches a page window edge.
struct ReferenceLine {
  std::string text;
  int indent = 0;
};

// The CJK paragraph style's display text of one complete physical line, written independently of the layout
// code: BOM, zero-width space and word joiner vanish; a run of whitespace (plain space, controls, Unicode
// spaces, NBSP, ideographic space) between two characters becomes one plain space, or the run's first NBSP or
// ideographic space when it holds no plain whitespace; whitespace at either end vanishes.
inline std::string cleanedReferenceLine(const std::string& line) {
  std::string out;
  std::string pending;  // the run seen since the last character, as it will be written
  size_t at = 0;
  while (at < line.size()) {
    uint32_t cp = 0;
    const size_t n = TxtLineBreak::decodeAt(line.data(), at, line.size(), cp);
    const std::string bytes = line.substr(at, n);
    at += n;
    if (cp == 0xFEFF || cp == 0x200B || cp == 0x2060) continue;
    const bool plain = cp == ' ' || (cp < 0x20 && cp != '\n') || cp == 0x7F || (cp >= 0x2000 && cp <= 0x200A) ||
                       cp == 0x202F || cp == 0x205F;
    if (plain) {
      pending = " ";
    } else if (cp == 0xA0 || cp == 0x3000) {
      if (pending.empty()) pending = bytes;
    } else {
      if (!out.empty()) out += pending;
      pending.clear();
      out += bytes;
    }
  }
  return out;
}

inline std::vector<ReferenceLine> referenceLines(const std::string& bytes, const Config& config) {
  const WidthOracle oracle(config.model);

  std::vector<ReferenceLine> out;
  size_t lineStart = 0;
  while (lineStart < bytes.size()) {
    const size_t newline = bytes.find('\n', lineStart);
    const size_t lineEnd = newline == std::string::npos ? bytes.size() : newline;
    size_t rawEnd = lineEnd;
    if (rawEnd > lineStart && bytes[rawEnd - 1] == '\r') rawEnd--;
    std::string line = bytes.substr(lineStart, rawEnd - lineStart);
    if (config.cjkParagraphs) line = cleanedReferenceLine(line);
    const size_t end = line.size();
    size_t s = 0;
    if (s == end) {
      if (!config.cjkParagraphs) out.push_back({});  // a blank line, which the CJK style drops
      lineStart = lineEnd + 1;
      continue;
    }
    const auto widthOf = [&](const size_t from, const size_t to) { return oracle.width(line.substr(from, to - from).c_str()); };
    const auto decode = [&](const size_t pos, const size_t limit, uint32_t& cp) {
      return TxtLineBreak::decodeAt(line.data(), pos, limit, cp);
    };
    const auto clusterEnd = [&](size_t pos, const size_t limit) {
      uint32_t cp = 0;
      pos += decode(pos, limit, cp);
      while (pos < limit) {
        const size_t n = decode(pos, limit, cp);
        if (!TxtLineBreak::isClusterExtender(cp)) break;
        pos += n;
      }
      return pos;
    };

    bool first = true;
    while (s < end) {
      const int indent = config.cjkParagraphs && first ? config.firstLineIndent : 0;
      const int width = config.viewportWidth - indent;
      size_t best = s;
      size_t bestNext = s;
      bool found = false;
      uint32_t prev = 0;
      size_t q = s + decode(s, end, prev);
      while (widthOf(s, q) <= width) {
        if (q == end) {
          best = end;
          bestNext = end;
          found = true;
          break;
        }
        uint32_t cp = 0;
        const size_t n = decode(q, end, cp);
        if (cp == ' ' || (prev != ' ' && TxtLineBreak::canBreakBetween(prev, cp))) {
          best = q;
          bestNext = cp == ' ' ? q + 1 : q;
          found = true;
        }
        prev = cp;
        q += n;
      }
      if (!found) {
        best = clusterEnd(s, end);
        while (best < end) {
          const size_t next = clusterEnd(best, end);
          if (widthOf(s, next) > width) break;
          best = next;
        }
        bestNext = best;
      }
      out.push_back({line.substr(s, best - s), indent});
      first = false;
      s = bestNext;
      // A continued line of the CJK style never starts with whitespace.
      while (config.cjkParagraphs && s < end) {
        uint32_t cp = 0;
        const size_t n = decode(s, end, cp);
        if (cp != ' ' && cp != 0xA0 && cp != 0x3000) break;
        s += n;
      }
    }
    lineStart = lineEnd + 1;
  }
  return out;
}

// ---- comparison ----

inline std::string describe(const char* what, const size_t page, const unsigned long long expected,
                            const unsigned long long actual) {
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), "%s differs at page %zu: expected %llu, production %llu", what, page,
                expected, actual);
  return buffer;
}

// Empty when `candidate` matches `expected` page for page.
inline std::string diffSweeps(const Sweep& expected, const Sweep& candidate, const bool compareLines) {
  if (expected.pages.size() != candidate.pages.size()) {
    return describe("page count", 0, expected.pages.size(), candidate.pages.size());
  }
  if (expected.completed != candidate.completed) {
    return describe("completion", 0, expected.completed, candidate.completed);
  }
  for (size_t i = 0; i < expected.pages.size(); i++) {
    const PageRecord& l = expected.pages[i];
    const PageRecord& c = candidate.pages[i];
    if (l.start != c.start) return describe("start", i, l.start, c.start);
    if (l.ok != c.ok) return describe("ok", i, l.ok, c.ok);
    if (l.next != c.next) return describe("next offset", i, l.next, c.next);
    if (l.lineCount != c.lineCount) return describe("line count", i, l.lineCount, c.lineCount);
    if (compareLines && l.lines != c.lines) return describe("line bytes", i, l.lines.size(), c.lines.size());
    if (l.prepared != c.prepared) return describe("prepared page digest", i, l.prepared.size(), c.prepared.size());
  }
  return {};
}

struct Access {
  size_t offset;
  bool reopenFirst;  // release and rebind the retained window before this load
};

// Loads arbitrary offsets in the given order, once through a fresh window per load and once through one
// retained window (forward slides, repeats, backward and distant jumps, reopen).
inline std::string diffAccessSequence(const std::string& bytes, const Config& config,
                                      const std::vector<Access>& accesses) {
  std::vector<PageRecord> expected;
  expected.reserve(accesses.size());
  installSource(bytes);
  {
    WidthOracle oracle(config.model);
    for (const Access& access : accesses) {
      TxtPagination::SourceWindow fresh;
      fresh.setSource(kSourcePath);
      expected.push_back(loadNewPage(fresh, bytes.size(), config, oracle, access.offset, true));
    }
  }

  installSource(bytes);
  WidthOracle oracle(config.model);
  TxtPagination::SourceWindow window;
  window.setSource(kSourcePath);
  for (size_t i = 0; i < accesses.size(); i++) {
    if (accesses[i].reopenFirst) {
      window.release();
      window.setSource(kSourcePath);
    }
    const PageRecord actual = loadNewPage(window, bytes.size(), config, oracle, accesses[i].offset, true);
    const PageRecord& l = expected[i];
    if (l.ok != actual.ok) return describe("ok", i, l.ok, actual.ok);
    if (l.next != actual.next) return describe("next offset", i, l.next, actual.next);
    if (l.lines != actual.lines) return describe("line bytes", i, l.lines.size(), actual.lines.size());
    if (l.indents != actual.indents) return describe("indents", i, l.indents.size(), actual.indents.size());
    if (l.prepared != actual.prepared) {
      return describe("prepared page digest", i, l.prepared.size(), actual.prepared.size());
    }
  }
  return {};
}

// ---- corpora ----

class Lcg {
 public:
  explicit Lcg(const uint64_t seed) : state_(seed) {}
  uint32_t next() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<uint32_t>(state_ >> 33);
  }
  uint32_t below(const uint32_t n) { return next() % n; }

 private:
  uint64_t state_;
};

// Words, multi-byte characters, blank lines, LF/CRLF/lone CR, runs of spaces, long unbroken runs and,
// optionally, invalid UTF-8 and NUL bytes.
inline std::string randomText(const uint32_t seed, const size_t targetBytes, const bool allowNul,
                              const bool allowInvalid = true) {
  static const char* const kWords[] = {"a",     "I",        "to",    "the",   "quick",   "brown",
                                       "fox",   "jumps",    "over",  "lazy",  "dogs",    "AVATAR",
                                       "fi",    "office",   "Total", "1234",  "x",       "wonderful",
                                       "ll",    "extraordinarily", "supercalifragilisticexpialidocious"};
  static const char* const kMultibyte[] = {"\xC3\xA9", "\xC3\xBC", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",
                                           "\xE4\xB8\xAD", "\xF0\x9F\x98\x80", "e\xCC\x81",
                                           "\xC3\x9F", "\xEF\xBB\xBF"};
  static const char* const kSeparators[] = {" ", " ", " ", "  ", "   ", "\n", "\n", "\r\n", "\n\n", "\r"};
  static const char* const kInvalid[] = {"\x80", "\xFF", "\xC3"};

  Lcg rng(seed);
  std::string out;
  out.reserve(targetBytes + 256);
  while (out.size() < targetBytes) {
    const uint32_t roll = rng.below(100);
    if (roll < 50) {
      out += kWords[rng.below(static_cast<uint32_t>(std::size(kWords)))];
    } else if (roll < 62) {
      out += kMultibyte[rng.below(static_cast<uint32_t>(std::size(kMultibyte)))];
    } else if (roll < 92) {
      out += kSeparators[rng.below(static_cast<uint32_t>(std::size(kSeparators)))];
    } else if (roll < 95) {
      out += allowInvalid ? kInvalid[rng.below(static_cast<uint32_t>(std::size(kInvalid)))] : "x";
    } else if (roll < 97) {
      out += allowNul ? std::string(1, '\0') : std::string(" ");
    } else {
      out.append(40 + rng.below(160), 'q');
    }
  }
  return out;
}

// Chinese prose: Han characters, CJK punctuation (closing marks that may not start a line, opening
// brackets that may not end one, doubled dashes and ellipses), a few Latin words and digits, 4-byte
// characters, ideographic-space indents and blank lines. Valid UTF-8 only.
inline std::string cjkText(const uint32_t seed, const size_t targetBytes, const size_t paragraphBytes) {
  static const char* const kHan[] = {"\xE7\x9A\x84", "\xE4\xB8\x80", "\xE6\x98\xAF", "\xE4\xBA\x86",
                                     "\xE6\x88\x91", "\xE4\xBB\x96", "\xE5\xA4\xA9", "\xE4\xB8\x8B",
                                     "\xE5\xB1\xB1", "\xE6\xB0\xB4", "\xE5\x89\x91", "\xE5\xBF\x83"};
  static const char* const kClosing[] = {"\xEF\xBC\x8C", "\xE3\x80\x82", "\xE3\x80\x81", "\xEF\xBC\x9B",
                                         "\xEF\xBC\x9A", "\xEF\xBC\x9F", "\xEF\xBC\x81", "\xE3\x80\x8D",
                                         "\xE3\x80\x8F", "\xEF\xBC\x89", "\xE3\x80\x91", "\xE3\x80\x8B",
                                         "\xE3\x80\x89", "\xE2\x80\xA6"};
  static const char* const kOpening[] = {"\xE3\x80\x8C", "\xE3\x80\x8E", "\xEF\xBC\x88", "\xE3\x80\x90",
                                         "\xE3\x80\x8A", "\xE3\x80\x88"};
  static const char* const kOther[] = {"\xE2\x80\x94\xE2\x80\x94", "\xE2\x80\xA6\xE2\x80\xA6", "Chapter",
                                       "2024", "\xF0\xA0\x80\x80", "OK"};

  Lcg rng(seed);
  std::string out;
  out.reserve(targetBytes + 64);
  size_t paragraph = 0;
  while (out.size() < targetBytes) {
    const uint32_t roll = rng.below(100);
    if (roll < 70) {
      out += kHan[rng.below(static_cast<uint32_t>(std::size(kHan)))];
    } else if (roll < 84) {
      out += kClosing[rng.below(static_cast<uint32_t>(std::size(kClosing)))];
    } else if (roll < 92) {
      out += kOpening[rng.below(static_cast<uint32_t>(std::size(kOpening)))];
    } else {
      out += kOther[rng.below(static_cast<uint32_t>(std::size(kOther)))];
    }
    if (paragraphBytes > 0 && out.size() - paragraph >= paragraphBytes) {
      out += rng.below(4) == 0 ? "\r\n\r\n" : "\n";
      out += rng.below(2) == 0 ? "\xE3\x80\x80\xE3\x80\x80" : "    ";
      paragraph = out.size();
    }
  }
  return out;
}

struct Book {
  std::string name;
  std::string bytes;
};

// Terminators and multi-byte characters straddling the 8192-byte page input edge, as the first line
// of a page and after another line.
inline std::vector<Book> boundaryBooks() {
  const std::string tail = "tail one\ntail two\n";
  const std::string multibyte[] = {"\xC3\xA9", "\xE6\x97\xA5", "\xF0\x9F\x98\x80"};
  std::vector<Book> books;
  for (size_t pos = 8186; pos <= 8198; pos++) {
    const std::string at = "@" + std::to_string(pos);
    books.push_back({"lf" + at, std::string(pos, 'a') + "\n" + tail});
    books.push_back({"crlf" + at, std::string(pos, 'a') + "\r\n" + tail});
    books.push_back({"lone-cr" + at, std::string(pos, 'a') + "\r" + tail});
    books.push_back({"after-head lf" + at, "head\n" + std::string(pos, 'b') + "\n" + tail});
    for (size_t m = 0; m < 3; m++) {
      const std::string name = "multibyte" + std::to_string(m) + at;
      books.push_back({name, std::string(pos, 'a') + multibyte[m] + "\n" + tail});
      books.push_back({"after-head " + name, "head\n" + std::string(pos, 'b') + multibyte[m] + "\n" + tail});
    }
  }
  return books;
}

// ---- reporting ----

inline void printCounters(const char* label, const Sweep& sweep) {
  const Counters& c = sweep.counters;
  std::printf(
      "[txt-pagination] %-30s pages=%zu opens=%d seeks=%d reads=%d requestedBytes=%zu actualBytes=%zu "
      "shortReads=%d errorReads=%d widthCalls=%llu widthBytes=%llu maxWidthBytes=%zu\n",
      label, sweep.pages.size(), c.opens, c.seeks, c.reads, c.requestedBytes, c.actualBytes, c.shortReads,
      c.errorReads, static_cast<unsigned long long>(c.widthCalls), static_cast<unsigned long long>(c.widthBytes),
      c.maxWidthBytes);
}

}  // namespace txt_replay
