#include <gtest/gtest.h>

#include <TxtChapters.h>
#include <TxtLineBreak.h>
#include <TxtPageIndexFormat.h>
#include <TxtPagination.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AllocationCounter.h"
#include "TxtReplay.h"

using namespace txt_replay;
using Model = WidthOracle::Model;
using TxtPagination::IndexStatus;
using TxtPagination::ReadStatus;

namespace {

using Lines = std::vector<std::string>;
using Indents = std::vector<int>;

std::string repeatText(const std::string& unit, const size_t count) {
  std::string out;
  out.reserve(unit.size() * count);
  for (size_t i = 0; i < count; i++) out += unit;
  return out;
}

std::string describeConfig(const Config& config) {
  std::string out = "viewport=" + std::to_string(config.viewportWidth) +
                    " lines=" + std::to_string(config.linesPerPage) +
                    (config.model == Model::Simple ? " simple" : " complex");
  if (config.cjkParagraphs) out += " cjk indent=" + std::to_string(config.firstLineIndent);
  return out;
}

Config cjkStyle(Config config, const int indent) {
  config.cjkParagraphs = true;
  config.firstLineIndent = indent;
  return config;
}

// Page-by-page lines, indents and next offsets, with and without line collection.
void expectPages(const std::string& bytes, const Config& config, const std::vector<Lines>& expectedLines,
                 const std::vector<size_t>& expectedNext, const std::vector<Indents>& expectedIndents = {}) {
  const Sweep collected = sweepNew(bytes, config, true);
  const Sweep counted = sweepNew(bytes, config, false);
  ASSERT_EQ(collected.pages.size(), expectedLines.size());
  ASSERT_EQ(counted.pages.size(), expectedLines.size());
  for (size_t i = 0; i < expectedLines.size(); i++) {
    SCOPED_TRACE("page " + std::to_string(i));
    EXPECT_EQ(collected.pages[i].lines, expectedLines[i]);
    EXPECT_EQ(collected.pages[i].next, expectedNext[i]);
    EXPECT_EQ(counted.pages[i].next, expectedNext[i]);
    EXPECT_EQ(counted.pages[i].lineCount, static_cast<int>(expectedLines[i].size()));
    if (expectedIndents.empty()) {
      EXPECT_EQ(collected.pages[i].indents, Indents(expectedLines[i].size(), 0));
    } else {
      EXPECT_EQ(collected.pages[i].indents, expectedIndents[i]);
    }
  }
  EXPECT_TRUE(collected.completed);
}

std::vector<ReferenceLine> producedLines(const Sweep& sweep) {
  std::vector<ReferenceLine> out;
  for (const PageRecord& page : sweep.pages) {
    for (size_t i = 0; i < page.lines.size(); i++) out.push_back({page.lines[i], page.indents[i]});
  }
  return out;
}

// The sweep's display lines are exactly the reference line breaking of the whole source. With `fullPages`,
// every page but the last also holds exactly linesPerPage lines (no page ended early).
void expectMatchesReference(const std::string& label, const std::string& bytes, const Config& config,
                            const bool fullPages = true) {
  SCOPED_TRACE(label + " " + describeConfig(config));
  const Sweep sweep = sweepNew(bytes, config, true);
  ASSERT_TRUE(sweep.completed);
  const std::vector<ReferenceLine> expected = referenceLines(bytes, config);
  const std::vector<ReferenceLine> actual = producedLines(sweep);
  const size_t common = std::min(expected.size(), actual.size());
  for (size_t i = 0; i < common; i++) {
    if (actual[i].text != expected[i].text || actual[i].indent != expected[i].indent) {
      ADD_FAILURE() << "display line " << i << " differs: production [" << actual[i].text << "] indent "
                    << actual[i].indent << ", reference [" << expected[i].text << "] indent " << expected[i].indent;
      return;
    }
  }
  EXPECT_EQ(actual.size(), expected.size());
  if (!fullPages) return;
  for (size_t i = 0; i + 1 < sweep.pages.size(); i++) {
    if (sweep.pages[i].lineCount != config.linesPerPage) {
      ADD_FAILURE() << "page " << i << " of " << sweep.pages.size() << " holds " << sweep.pages[i].lineCount
                    << " lines";
      return;
    }
  }
}

// Every byte of every page is on one of its display lines, except line terminators, the space a line broke
// at and (CJK paragraph style) stripped indentation; pages tile the source; no line splits a code point.
void expectNoLostBytes(const std::string& label, const std::string& bytes, const Config& config) {
  SCOPED_TRACE(label + " " + describeConfig(config));
  const Sweep sweep = sweepNew(bytes, config, true);
  ASSERT_TRUE(sweep.completed);
  size_t pageStart = 0;
  for (size_t p = 0; p < sweep.pages.size(); p++) {
    const PageRecord& page = sweep.pages[p];
    ASSERT_EQ(page.start, pageStart);
    ASSERT_GT(page.next, page.start);
    const auto skip = [&](size_t& at) {
      if (at >= page.next) return false;
      const char c = bytes[at];
      if (c == '\n' || c == '\r' || c == ' ') {
        at++;
        return true;
      }
      if (!config.cjkParagraphs) return false;
      uint32_t cp = 0;
      const size_t n = TxtLineBreak::decodeAt(bytes.data(), at, bytes.size(), cp);
      if (!TxtLineBreak::isParagraphIndentSpace(cp)) return false;
      at += n;
      return true;
    };
    size_t cursor = page.start;
    for (const std::string& line : page.lines) {
      while (bytes.compare(cursor, line.size(), line) != 0) {
        if (!skip(cursor)) {
          ADD_FAILURE() << "page " << p << ": source bytes at " << cursor << " are on no display line";
          return;
        }
      }
      cursor += line.size();
      for (size_t i = 0; i < line.size();) {
        uint32_t cp = 0;
        i += TxtLineBreak::decodeAt(line.data(), i, line.size(), cp);
        if (cp == TxtLineBreak::REPLACEMENT) {
          ADD_FAILURE() << "page " << p << ": a display line ending at " << cursor << " splits a code point";
          return;
        }
      }
    }
    ASSERT_LE(cursor, page.next) << "page " << p;
    while (cursor < page.next) {
      if (!skip(cursor)) {
        ADD_FAILURE() << "page " << p << ": source bytes at " << cursor << " are on no display line";
        return;
      }
    }
    pageStart = page.next;
  }
  EXPECT_EQ(pageStart, bytes.size());
}

uint32_t firstCodepoint(const std::string& s) {
  uint32_t cp = 0;
  if (!s.empty()) TxtLineBreak::decodeAt(s.data(), 0, s.size(), cp);
  return cp;
}

uint32_t lastCodepoint(const std::string& s) {
  if (s.empty()) return 0;
  size_t lead = s.size() - 1;
  while (lead > 0 && (static_cast<uint8_t>(s[lead]) & 0xC0) == 0x80) lead--;
  uint32_t cp = 0;
  TxtLineBreak::decodeAt(s.data(), lead, s.size(), cp);
  return cp;
}

// 。，、；：？！」』）】》〉… never start a line; 「『（【《〈 never end one.
constexpr uint32_t kNoLineStart[] = {0x3002, 0xFF0C, 0x3001, 0xFF1B, 0xFF1A, 0xFF1F, 0xFF01,
                                     0x300D, 0x300F, 0xFF09, 0x3011, 0x300B, 0x3009, 0x2026};
constexpr uint32_t kNoLineEnd[] = {0x300C, 0x300E, 0xFF08, 0x3010, 0x300A, 0x3008};

bool isOneOf(const uint32_t cp, const uint32_t* begin, const uint32_t* end) { return std::find(begin, end, cp) != end; }

bool isAsciiAlnum(const uint32_t cp) {
  return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9');
}

// Continuation lines of a CJK-style sweep (indent 0) obey kinsoku, keep Latin words and doubled marks whole.
void expectKinsoku(const std::string& label, const Sweep& sweep) {
  SCOPED_TRACE(label);
  const std::vector<ReferenceLine> lines = producedLines(sweep);
  size_t continuations = 0;
  for (size_t i = 1; i < lines.size(); i++) {
    if (lines[i].indent != 0) continue;  // a paragraph's first line
    continuations++;
    const uint32_t first = firstCodepoint(lines[i].text);
    const uint32_t last = lastCodepoint(lines[i - 1].text);
    ASSERT_FALSE(isOneOf(first, std::begin(kNoLineStart), std::end(kNoLineStart)))
        << "line " << i << " starts with U+" << std::hex << first;
    ASSERT_FALSE(isOneOf(last, std::begin(kNoLineEnd), std::end(kNoLineEnd)))
        << "line " << i - 1 << " ends with U+" << std::hex << last;
    ASSERT_FALSE(isAsciiAlnum(last) && isAsciiAlnum(first)) << "a Latin word is split before line " << i;
    ASSERT_FALSE(first == 0x2014 && last == 0x2014) << "—— is split before line " << i;
  }
  EXPECT_GT(continuations, 0u);
}

constexpr uint64_t kFnvBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

struct HashSink {
  uint64_t hash = kFnvBasis;
  size_t lines = 0;
};

void foldLine(HashSink& sink, const char* text, const size_t length) {
  for (size_t i = 0; i < length; i++) sink.hash = (sink.hash ^ static_cast<uint8_t>(text[i])) * kFnvPrime;
  sink.hash = (sink.hash ^ 0x0A) * kFnvPrime;
  sink.lines++;
}

void hashLine(void* ctx, const char* text, const size_t length, int) {
  foldLine(*static_cast<HashSink*>(ctx), text, length);
}

struct SinkProbe {
  int calls = 0;
};
void countingSink(void* ctx, const char*, size_t, int) { ++static_cast<SinkProbe*>(ctx)->calls; }

struct MeasureProbe {
  int prepares = 0;
  int widths = 0;
};
void probePrepare(void* ctx, const char*) { ++static_cast<MeasureProbe*>(ctx)->prepares; }
int probeWidth(void* ctx, const char*) {
  ++static_cast<MeasureProbe*>(ctx)->widths;
  return 0;
}

// Per-code-point advances from the same oracle, as the reader's advance cache supplies them.
int oracleCodepointAdvance(void* ctx, const uint32_t cp) {
  char utf8[5] = {};
  if (cp < 0x80) {
    utf8[0] = static_cast<char>(cp);
  } else if (cp < 0x800) {
    utf8[0] = static_cast<char>(0xC0 | (cp >> 6));
    utf8[1] = static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    utf8[0] = static_cast<char>(0xE0 | (cp >> 12));
    utf8[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    utf8[2] = static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    utf8[0] = static_cast<char>(0xF0 | (cp >> 18));
    utf8[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    utf8[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    utf8[3] = static_cast<char>(0x80 | (cp & 0x3F));
  }
  return static_cast<NewAdapter*>(ctx)->oracle->width(utf8);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Code points and break classes
// ---------------------------------------------------------------------------------------------

TEST(TxtLineBreak, DecodingConsumesEveryByteExactlyOnce) {
  const std::string text = std::string("a") + "\xC3\xA9" + "\xE4\xB8\xAD" + "\xF0\x9F\x98\x80" + "\x80" + "\xC3";
  std::vector<uint32_t> codepoints;
  std::vector<size_t> sizes;
  for (size_t pos = 0; pos < text.size();) {
    uint32_t cp = 0;
    const size_t n = TxtLineBreak::decodeAt(text.data(), pos, text.size(), cp);
    codepoints.push_back(cp);
    sizes.push_back(n);
    pos += n;
  }
  EXPECT_EQ(codepoints, (std::vector<uint32_t>{'a', 0xE9, 0x4E2D, 0x1F600, 0xFFFD, 0xFFFD}));
  EXPECT_EQ(sizes, (std::vector<size_t>{1, 2, 3, 4, 1, 1}));
}

TEST(TxtLineBreak, CompletePrefixStopsBeforeACutCodePoint) {
  const std::string emoji = "\xF0\x9F\x98\x80";
  for (size_t cut = 0; cut <= 4; cut++) {
    const std::string text = "ab" + emoji.substr(0, cut);
    EXPECT_EQ(TxtLineBreak::completePrefix(text.data(), text.size()), cut == 4 ? size_t{6} : size_t{2}) << cut;
  }
  const std::string han = "\xE4\xB8\xAD";
  for (size_t cut = 1; cut <= 3; cut++) {
    const std::string text = "x" + han.substr(0, cut);
    EXPECT_EQ(TxtLineBreak::completePrefix(text.data(), text.size()), cut == 3 ? size_t{4} : size_t{1}) << cut;
  }
  // Bytes that can never start a sequence stay: they decode as U+FFFD.
  EXPECT_EQ(TxtLineBreak::completePrefix("ab\x80", 3), 3u);
}

TEST(TxtLineBreak, CjkDominantBooksGetTheParagraphStyle) {
  const auto dominant = [](const std::string& s) { return TxtLineBreak::isCjkDominant(s.data(), s.size()); };
  const std::string han20 = repeatText("中", 20);
  EXPECT_TRUE(dominant(han20));
  EXPECT_TRUE(dominant(han20 + std::string(80, 'a')));
  EXPECT_FALSE(dominant(han20 + std::string(81, 'a')));
  EXPECT_FALSE(dominant(repeatText("中", 15)));
  EXPECT_FALSE(dominant("The quick brown fox jumps over the lazy dog.\n"));
  EXPECT_TRUE(dominant(cjkText(7, 4000, 300)));
}

// ---------------------------------------------------------------------------------------------
// Goldens: simple oracle, 10px per character, 20px for wide characters; viewport 100 = 10 characters.
// ---------------------------------------------------------------------------------------------

TEST(TxtPaginationGolden, WrapsAtSpacesAndContinuesAcrossPages) {
  expectPages("hello brave new world\nab\n\nxy\nlast line here\n", {100, 4, Model::Simple},
              {Lines{"hello", "brave new", "world", "ab"}, Lines{"", "xy", "last line", "here"}}, {25, 44});
}

TEST(TxtPaginationGolden, CrlfIsStrippedAndOnlyOneSeparatorSpaceIsSkipped) {
  expectPages("aaaa  bbbb cc\r\ndd\r\n", {100, 10, Model::Simple}, {Lines{"aaaa  bbbb", "cc", "dd"}}, {19});
  expectPages("abcdefghij   klm", {100, 10, Model::Simple}, {Lines{"abcdefghij", "  klm"}}, {16});
}

TEST(TxtPaginationGolden, LeadingAndTrailingSpacesAreContent) {
  expectPages("   lead\n  \ntrail   \n", {100, 10, Model::Simple}, {Lines{"   lead", "  ", "trail   "}}, {20});
}

TEST(TxtPaginationGolden, LongWordSplitsAtTheViewport) {
  expectPages("abcdefghijklmnopqrstuvwxyz", {100, 10, Model::Simple},
              {Lines{"abcdefghij", "klmnopqrst", "uvwxyz"}}, {26});
}

TEST(TxtPaginationGolden, WholeMultibyteCharactersAreKeptTogether) {
  // a, 2-byte, 3-byte (wide), 4-byte (wide), b: widths 10, 10, 20, 20, 10 at viewport 30.
  const std::string text = std::string("a") + "\xC3\xA9" + "\xE6\x97\xA5" + "\xF0\x9F\x98\x80" + "b";
  expectPages(text, {30, 10, Model::Simple}, {Lines{"a\xC3\xA9", "\xE6\x97\xA5", "\xF0\x9F\x98\x80" "b"}}, {11});
}

TEST(TxtPaginationGolden, AGlyphWiderThanTheViewportStaysWhole) {
  expectPages("abc", {5, 10, Model::Simple}, {Lines{"a", "b", "c"}}, {3});
  expectPages("\xC3\xA9", {5, 10, Model::Simple}, {Lines{"\xC3\xA9"}}, {2});
  expectPages("e\xCC\x81" "x", {5, 10, Model::Simple}, {Lines{"e\xCC\x81", "x"}}, {4});
  expectPages("中", {5, 10, Model::Simple}, {Lines{"中"}}, {3});
}

TEST(TxtPaginationGolden, LinesPerPageCanEndAPageInsideAPhysicalLine) {
  expectPages("aaaa bbbb cccc dddd eeee ffff", {100, 2, Model::Simple},
              {Lines{"aaaa bbbb", "cccc dddd"}, Lines{"eeee ffff"}}, {20, 29});
}

TEST(TxtPaginationGolden, BlankEmptyAndUnterminatedInputs) {
  const Config config{100, 10, Model::Simple};
  expectPages("", config, {}, {});
  expectPages("\n", config, {Lines{""}}, {1});
  expectPages("\n\n", config, {Lines{"", ""}}, {2});
  expectPages("a", config, {Lines{"a"}}, {1});
  expectPages("ab\n", config, {Lines{"ab"}}, {3});
  expectPages("ab", config, {Lines{"ab"}}, {2});
  expectPages("\r\n", config, {Lines{""}}, {2});
  expectPages("\r", config, {Lines{""}}, {1});
  expectPages("a\rb\n", config, {Lines{"a\rb"}}, {4});
}

TEST(TxtPaginationGolden, PageInputLengthIsBounded) {
  EXPECT_EQ(TxtPagination::pageInputLength(0, 8191), 8191u);
  EXPECT_EQ(TxtPagination::pageInputLength(0, 8192), 8192u);
  EXPECT_EQ(TxtPagination::pageInputLength(0, 8193), 8192u);
  EXPECT_EQ(TxtPagination::pageInputLength(8192, 8193), 1u);
  EXPECT_EQ(TxtPagination::pageInputLength(5, 5), 0u);
  EXPECT_EQ(TxtPagination::pageInputLength(6, 5), 0u);
}

// ---------------------------------------------------------------------------------------------
// The 8KB page input edge: no byte is lost or duplicated and no code point is split.
// ---------------------------------------------------------------------------------------------

TEST(TxtPaginationEdge, TerminatorsAtTheEdge) {
  const Config config{100000, 40, Model::Simple};
  // LF is the last byte of the page input: the page ends exactly there.
  expectPages(std::string(8191, 'a') + "\ntail\n", config, {Lines{std::string(8191, 'a')}, Lines{"tail"}},
              {8192, 8197});
  // LF is the first byte past the page input: the next page starts on it and shows no extra blank line.
  expectPages(std::string(8192, 'a') + "\ntail\n", config, {Lines{std::string(8192, 'a')}, Lines{"tail"}},
              {8192, 8198});
  // CR is the last byte of the page input, LF the first past it.
  expectPages(std::string(8191, 'a') + "\r\nx\n", config, {Lines{std::string(8191, 'a')}, Lines{"x"}},
              {8191, 8195});
}

TEST(TxtPaginationEdge, ALineLongerThanThePageInputContinuesOnTheNextPage) {
  const Config config{100000, 40, Model::Simple};
  expectPages(std::string(8200, 'a') + "\nz\n", config, {Lines{std::string(8192, 'a')}, Lines{"aaaaaaaa", "z"}},
              {8192, 8203});
}

TEST(TxtPaginationEdge, ACodePointCutByTheEdgeMovesWholeToTheNextPage) {
  const Config config{100000, 40, Model::Simple};
  expectPages(std::string(8191, 'a') + "\xC3\xA9" + "\nx\n", config,
              {Lines{std::string(8191, 'a')}, Lines{"\xC3\xA9", "x"}}, {8191, 8196});
  expectPages(std::string(8190, 'a') + "\xF0\x9F\x98\x80" + "\nx\n", config,
              {Lines{std::string(8190, 'a')}, Lines{"\xF0\x9F\x98\x80", "x"}}, {8190, 8197});
  expectPages(std::string(8191, 'a') + "\xF0\x9F\x98\x80" + "\nx\n", config,
              {Lines{std::string(8191, 'a')}, Lines{"\xF0\x9F\x98\x80", "x"}}, {8191, 8198});
}

TEST(TxtPaginationEdge, AShortLineCutByTheEdgeMovesWholeToTheNextPage) {
  const Config config{100000, 40, Model::Simple};
  expectPages(std::string(8186, 'a') + "\ntail one\ntail two\n", config,
              {Lines{std::string(8186, 'a')}, Lines{"tail one", "tail two"}}, {8187, 8205});
}

TEST(TxtPaginationEdge, BoundaryBooksLoseNoBytes) {
  for (const Book& book : boundaryBooks()) {
    expectNoLostBytes(book.name, book.bytes, {100000, 40, Model::Simple});
    expectNoLostBytes(book.name, book.bytes, {100000, 3, Model::Simple});
    expectNoLostBytes(book.name, book.bytes, cjkStyle({100000, 3, Model::Simple}, 40));
    if (HasFailure()) return;
  }
}

TEST(TxtPaginationEdge, HanTextAtEveryAlignmentLosesNoBytes) {
  for (size_t shift = 0; shift < 4; shift++) {
    // One unbroken paragraph: every page is a whole window, cut before a split code point.
    const std::string single = std::string(shift, 'a') + cjkText(static_cast<uint32_t>(20 + shift), 40000, 0);
    expectNoLostBytes("single paragraph shift " + std::to_string(shift), single, {100000, 40, Model::Simple});
    expectNoLostBytes("single paragraph shift " + std::to_string(shift), single,
                      cjkStyle({100000, 40, Model::Simple}, 40));
    // Paragraphs of about one window.
    const std::string paragraphs = std::string(shift, 'a') + cjkText(static_cast<uint32_t>(30 + shift), 60000, 7000);
    expectNoLostBytes("paragraphs shift " + std::to_string(shift), paragraphs, {100000, 3, Model::Simple});
    expectNoLostBytes("paragraphs shift " + std::to_string(shift), paragraphs,
                      cjkStyle({100000, 40, Model::Simple}, 40));
  }
}

// ---------------------------------------------------------------------------------------------
// CJK line breaking
// ---------------------------------------------------------------------------------------------

TEST(TxtPaginationCjk, BreaksBetweenCharactersWithKinsoku) {
  // Closing punctuation never starts a line.
  expectPages("中文。", {40, 10, Model::Simple}, {Lines{"中", "文。"}}, {9});
  // An opening bracket never ends a line.
  expectPages("我说「好」", {60, 10, Model::Simple}, {Lines{"我说", "「好」"}}, {15});
  // Latin words inside Chinese text stay whole; breaks fall on the CJK side.
  expectPages("我爱Python语言", {80, 10, Model::Simple}, {Lines{"我爱", "Python语", "言"}}, {18});
  // Doubled dashes and ellipses stay together.
  expectPages("他说——好", {50, 10, Model::Simple}, {Lines{"他说", "——好"}}, {15});
  expectPages("他走了……", {60, 10, Model::Simple}, {Lines{"他走", "了……"}}, {15});
  // A run with no allowed break is cut where it must be.
  expectPages("「「「「「「", {40, 10, Model::Simple}, {Lines{"「「", "「「", "「「"}}, {18});
}

TEST(TxtPaginationCjk, ChineseProseMatchesTheReference) {
  for (uint32_t seed = 1; seed <= 12; seed++) {
    const std::string paragraphs = cjkText(seed, 20000 + seed * 1500, 300 + seed * 40);
    const std::string label = "cjk seed " + std::to_string(seed);
    expectMatchesReference(label, paragraphs, {440, 26, Model::Simple});
    expectMatchesReference(label, paragraphs, cjkStyle({440, 26, Model::Simple}, 40));
    expectMatchesReference(label, paragraphs, cjkStyle({300, 40, Model::Simple}, 20));
    expectMatchesReference(label, paragraphs, cjkStyle({600, 20, Model::Simple}, 40));
  }
}

TEST(TxtPaginationCjk, A200KiBSingleParagraphMatchesTheReference) {
  const std::string paragraph = cjkText(42, 200 * 1024, 0);
  ASSERT_GE(paragraph.size(), 200u * 1024u);
  ASSERT_EQ(paragraph.find('\n'), std::string::npos);
  expectMatchesReference("200 KiB paragraph", paragraph, {440, 26, Model::Simple});
  expectMatchesReference("200 KiB paragraph", paragraph, cjkStyle({440, 26, Model::Simple}, 40));
  expectKinsoku("200 KiB paragraph", sweepNew(paragraph, cjkStyle({440, 26, Model::Simple}, 40), true));
}

TEST(TxtPaginationCjk, ProseObeysKinsoku) {
  for (uint32_t seed = 1; seed <= 8; seed++) {
    const std::string bytes = cjkText(seed, 30000, 200 + seed * 150);
    for (const Config& config : {cjkStyle({440, 26, Model::Simple}, 40), cjkStyle({600, 20, Model::Simple}, 40),
                                 cjkStyle({440, 26, Model::Complex}, 40)}) {
      expectKinsoku("cjk seed " + std::to_string(seed) + " " + describeConfig(config), sweepNew(bytes, config, true));
    }
  }
}

TEST(TxtPaginationCjk, CodepointAdvancesOnlyEstimateTheLine) {
  // The reader's per-code-point advance cache must not change any page, only save whole-line measurements.
  const std::vector<std::pair<std::string, Config>> subjects = {
      {cjkText(5, 40000, 500), cjkStyle({440, 26, Model::Simple}, 40)},
      {cjkText(6, 40000, 0), {300, 40, Model::Simple}},
      {randomText(7, 30000, false, false), {300, 40, Model::Simple}},
      {randomText(8, 30000, false, false), cjkStyle({120, 17, Model::Simple}, 20)},
  };
  for (const auto& [bytes, config] : subjects) {
    SCOPED_TRACE(describeConfig(config));
    const Sweep plain = sweepNew(bytes, config, true);
    ASSERT_TRUE(plain.completed);

    installSource(bytes);
    WidthOracle oracle(config.model);
    NewAdapter adapter{&oracle, nullptr};
    TxtPagination::PageLayout layout = makeLayout(config, adapter);
    layout.measure.codepointAdvance = oracleCodepointAdvance;
    TxtPagination::SourceWindow window;
    window.setSource(kSourcePath);
    size_t offset = 0;
    for (const PageRecord& expected : plain.pages) {
      PageRecord page;
      const TxtPagination::LineSink sink{&page, collectLine};
      const TxtPagination::PageResult result = TxtPagination::loadPage(window, offset, bytes.size(), layout, &sink);
      ASSERT_EQ(result.status, ReadStatus::Ok);
      ASSERT_EQ(result.nextOffset, expected.next);
      ASSERT_EQ(page.lines, expected.lines);
      ASSERT_EQ(page.indents, expected.indents);
      offset = result.nextOffset;
    }
    EXPECT_EQ(offset, bytes.size());
  }
}

// ---------------------------------------------------------------------------------------------
// Chinese paragraph style
// ---------------------------------------------------------------------------------------------

TEST(TxtPaginationParagraphStyle, IndentsStripsLeadingWhitespaceAndDropsBlankLines) {
  const std::string text = "　　甲乙丙丁戊\n\n\n  己庚。\r\n\r\n\t辛\n";
  expectPages(text, cjkStyle({100, 10, Model::Simple}, 40), {Lines{"甲乙丙", "丁戊", "己庚。", "辛"}}, {text.size()},
              {Indents{40, 0, 40, 40}});
  // A byte-order mark is leading whitespace too.
  expectPages("\xEF\xBB\xBF" "第一章\n", cjkStyle({200, 10, Model::Simple}, 40), {Lines{"第一章"}}, {13},
              {Indents{40}});
}

TEST(TxtPaginationParagraphStyle, OnlyAParagraphsFirstLineIsIndented) {
  const Config config = cjkStyle({100, 1, Model::Simple}, 40);
  // A paragraph continued on the next page is not indented again.
  expectPages("甲乙丙丁戊", config, {Lines{"甲乙丙"}, Lines{"丁戊"}}, {9, 15}, {Indents{40}, Indents{0}});
  // A page that starts a paragraph indents it.
  expectPages("甲乙丙\n丁戊", config, {Lines{"甲乙丙"}, Lines{"丁戊"}}, {10, 16}, {Indents{40}, Indents{40}});
  expectPages("　　甲乙\n　　丙丁\n", config, {Lines{"甲乙"}, Lines{"丙丁"}}, {13, 26}, {Indents{40}, Indents{40}});
}

TEST(TxtPaginationParagraphStyle, ByteOffsetsStayExact) {
  const Config config = cjkStyle({100, 1, Model::Simple}, 40);
  // Blank lines take no display line but their bytes stay on a page.
  expectPages("甲\n\n\n\n乙", config, {Lines{"甲"}, Lines{"乙"}}, {4, 10}, {Indents{40}, Indents{40}});
  // Trailing blank lines are a page with no lines, which the index folds into the page before.
  expectPages("甲\n　　", config, {Lines{"甲"}, Lines{}}, {4, 10}, {Indents{40}, Indents{}});
}

TEST(TxtPaginationParagraphStyle, OtherBooksKeepTheirWhitespace) {
  const std::string text = "  Hello world\n\n\nNext\n";
  expectPages(text, {1000, 10, Model::Simple}, {Lines{"  Hello world", "", "", "Next"}}, {text.size()});
}

// ---------------------------------------------------------------------------------------------
// Reference line breaking
// ---------------------------------------------------------------------------------------------

TEST(TxtPaginationReference, RandomTextMatchesTheReference) {
  const std::vector<Config> configs = {{440, 26, Model::Simple}, {300, 40, Model::Simple}, {120, 17, Model::Simple},
                                       {40, 9, Model::Simple},   {12, 5, Model::Simple},   {480, 1, Model::Simple}};
  for (uint32_t seed = 1; seed <= 36; seed++) {
    const std::string bytes = randomText(seed, 3000 + (seed * 337) % 17000, false, false);
    const std::string label = "random seed " + std::to_string(seed);
    expectMatchesReference(label, bytes, configs[seed % configs.size()]);
    expectMatchesReference(label, bytes, configs[(seed + 3) % configs.size()]);
    expectMatchesReference(label, bytes, cjkStyle(configs[(seed + 1) % configs.size()], 20));
  }
}

TEST(TxtPaginationReference, BoundaryBooksMatchTheReference) {
  for (const Book& book : boundaryBooks()) {
    expectMatchesReference(book.name, book.bytes, {440, 26, Model::Simple});
    if (HasFailure()) return;
  }
}

TEST(TxtPaginationReference, LinesLongerThanThePageInputMatchTheReference) {
  std::string words;
  while (words.size() < 8300) words += "word ";
  expectMatchesReference("overlong first line", words + "\nz\n", {6000, 40, Model::Simple}, false);
  expectMatchesReference("overlong line after a line", "first\n" + words + "\nz\n", {6000, 40, Model::Simple}, false);
  const std::string han = cjkText(9, 30000, 0);
  expectMatchesReference("overlong han line after a line", "first\n" + han + "\nz\n", {4000, 40, Model::Simple},
                         false);
}

// ---------------------------------------------------------------------------------------------
// Walker invariants
// ---------------------------------------------------------------------------------------------

TEST(TxtPaginationWalker, RestoresEveryByteItBorrowsForWidthQueries) {
  const std::string notes = notesFixture();
  const Config config{300, 12, Model::Complex};
  const std::vector<std::string> pages = {notes.substr(0, TxtPagination::PAGE_INPUT_BYTES), randomText(11, 6000, false),
                                          randomText(12, 6000, true), cjkText(13, 6000, 400)};
  for (const std::string& page : pages) {
    std::vector<char> buffer(page.begin(), page.end());
    buffer.push_back('\0');
    const std::vector<char> original = buffer;
    WidthOracle oracle(config.model);
    NewAdapter adapter{&oracle, nullptr};
    const TxtPagination::PageLayout layout = makeLayout(cjkStyle(config, 24), adapter);
    size_t next = 0;
    const int lines =
        TxtPagination::layoutPage(buffer.data(), page.size(), 0, notes.size(), true, layout, nullptr, nullptr, next);
    EXPECT_GT(lines, 0);
    EXPECT_GT(oracle.calls, 0u);
    EXPECT_EQ(buffer, original);
  }
}

TEST(TxtPaginationWalker, CountOnlyAndCollectedModesReportTheSameLines) {
  for (const Config& config : {Config{440, 26, Model::Complex}, cjkStyle({440, 26, Model::Complex}, 40)}) {
    const std::string notes = notesFixture();
    const Sweep collected = sweepNew(notes, config, true);
    const Sweep counted = sweepNew(notes, config, false);
    ASSERT_EQ(collected.pages.size(), counted.pages.size());
    for (size_t i = 0; i < collected.pages.size(); i++) {
      EXPECT_EQ(collected.pages[i].next, counted.pages[i].next);
      EXPECT_EQ(collected.pages[i].lineCount, counted.pages[i].lineCount);
      EXPECT_EQ(static_cast<size_t>(collected.pages[i].lineCount), collected.pages[i].lines.size());
      EXPECT_TRUE(counted.pages[i].lines.empty());
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Source window
// ---------------------------------------------------------------------------------------------

class TxtPaginationWindow : public ::testing::Test {
 protected:
  void SetUp() override {
    data.resize(20000);
    for (size_t i = 0; i < data.size(); i++) data[i] = static_cast<char>((i * 131 + 7) & 0xFF);
    installSource(data);
    window.setSource(kSourcePath);
  }

  FakeSource& io() { return Storage.source(); }

  void expectLoad(const size_t offset, const size_t length) {
    char* text = nullptr;
    ASSERT_EQ(window.load(offset, length, text), ReadStatus::Ok);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(std::memcmp(text, data.data() + offset, length), 0);
    EXPECT_EQ(text[length], '\0');
  }

  void expectIo(const int opens, const int seeks, const int reads, const size_t bytes) {
    EXPECT_EQ(io().openCount, opens);
    EXPECT_EQ(io().seekCount, seeks);
    EXPECT_EQ(io().readCalls, reads);
    EXPECT_EQ(io().requestedBytes, bytes);
    EXPECT_EQ(io().actualBytes, bytes);
  }

  std::string data;
  TxtPagination::SourceWindow window;
};

TEST_F(TxtPaginationWindow, ForwardLoadsReadOnlyTheNewlyExposedSuffix) {
  expectLoad(0, 8192);
  expectIo(1, 0, 1, 8192);
  expectLoad(100, 8192);  // keeps 8092 bytes, reads 100
  expectIo(1, 0, 2, 8292);
  expectLoad(100, 8192);  // same range again: no IO
  expectIo(1, 0, 2, 8292);
  expectLoad(50, 8192);  // backward: seek and refill
  expectIo(1, 1, 3, 16484);
  expectLoad(8242, 5000);  // starts exactly where the window ends: sequential read, no seek
  expectIo(1, 1, 4, 21484);
  expectLoad(19000, 1000);  // distant jump
  expectIo(1, 2, 5, 22484);
  expectLoad(19500, 500);  // wholly inside the window: no IO
  expectIo(1, 2, 5, 22484);
  expectLoad(0, TxtPagination::SourceWindow::WINDOW_BYTES);  // from the end back to the start
  expectIo(1, 3, 6, 22484 + TxtPagination::SourceWindow::WINDOW_BYTES);
  EXPECT_EQ(io().openWhileOpen, 0);
}

TEST_F(TxtPaginationWindow, EveryLoadMatchesAFreshReadForAnyAccessOrder) {
  Lcg rng(7);
  size_t prevOffset = 0;
  size_t prevLength = 0;
  for (int i = 0; i < 3000; i++) {
    size_t offset = 0;
    switch (rng.below(5)) {
      case 0:
        offset = prevOffset + rng.below(static_cast<uint32_t>(prevLength + 1));  // slide forward inside the window
        break;
      case 1:
        offset = prevOffset;
        break;
      case 2:
        offset = prevOffset > 0 ? rng.below(static_cast<uint32_t>(prevOffset)) : 0;  // backward
        break;
      case 3:
        offset = rng.below(static_cast<uint32_t>(data.size()));
        break;
      default:
        offset = prevOffset + prevLength;  // exactly adjacent
        break;
    }
    if (offset >= data.size()) offset = data.size() - 1;
    const size_t maxLength = std::min<size_t>(TxtPagination::SourceWindow::WINDOW_BYTES, data.size() - offset);
    const size_t length = 1 + rng.below(static_cast<uint32_t>(maxLength));
    if (rng.below(50) == 0) {
      window.release();
      window.setSource(kSourcePath);
    }
    expectLoad(offset, length);
    if (HasFatalFailure()) return;
    prevOffset = offset;
    prevLength = length;
  }
  EXPECT_EQ(io().openWhileOpen, 0);
  EXPECT_EQ(io().shortReads, 0);
  EXPECT_EQ(io().errorReads, 0);
}

TEST_F(TxtPaginationWindow, ReleaseClosesTheHandleAndTheNextLoadReopens) {
  expectLoad(0, 4096);
  window.release();
  expectLoad(100, 4096);
  EXPECT_EQ(io().openCount, 2);
  EXPECT_EQ(io().openWhileOpen, 0);
}

TEST_F(TxtPaginationWindow, RejectsRangesOutsideTheWindowBoundWithoutTouchingTheSource) {
  char dummy = 0;
  char* text = &dummy;
  EXPECT_EQ(window.load(0, 0, text), ReadStatus::InvalidRange);
  EXPECT_EQ(text, nullptr);
  text = &dummy;
  EXPECT_EQ(window.load(0, TxtPagination::SourceWindow::WINDOW_BYTES + 1, text), ReadStatus::InvalidRange);
  EXPECT_EQ(text, nullptr);
  EXPECT_EQ(io().openCount, 0);
  EXPECT_EQ(io().readCalls, 0);
}

TEST_F(TxtPaginationWindow, ReadErrorFailsAndTheNextLoadReopensWithExactBytes) {
  io().errorOnReadCall = 1;
  char dummy = 0;
  char* text = &dummy;
  EXPECT_EQ(window.load(0, 8192, text), ReadStatus::ReadError);
  EXPECT_EQ(text, nullptr);
  io().errorOnReadCall = 0;
  expectLoad(0, 8192);
  EXPECT_EQ(io().openCount, 2);
  EXPECT_EQ(io().errorReads, 1);
}

TEST_F(TxtPaginationWindow, ShortReadIsReportedAndNoPartialWindowSurvives) {
  expectLoad(0, 200);
  io().truncateAt = 250;
  char dummy = 0;
  char* text = &dummy;
  // Keeps 100 resident bytes, then the source ends after 50 more: 8192 cannot be filled.
  EXPECT_EQ(window.load(100, 8192, text), ReadStatus::ShortRead);
  EXPECT_EQ(text, nullptr);
  EXPECT_GE(io().shortReads, 1);
  io().truncateAt = std::numeric_limits<size_t>::max();
  expectLoad(100, 8192);  // would expose unread bytes if the failed fill had been kept
}

TEST_F(TxtPaginationWindow, ZeroByteReadIsAShortRead) {
  io().zeroOnReadCall = 1;
  char dummy = 0;
  char* text = &dummy;
  EXPECT_EQ(window.load(0, 4096, text), ReadStatus::ShortRead);
  EXPECT_EQ(text, nullptr);
}

TEST_F(TxtPaginationWindow, OversizedReadCountIsAnError) {
  io().oversizeOnReadCall = 1;
  char dummy = 0;
  char* text = &dummy;
  EXPECT_EQ(window.load(0, 4096, text), ReadStatus::ReadError);
  EXPECT_EQ(text, nullptr);
}

TEST_F(TxtPaginationWindow, ShortProgressIsRetriedUntilTheRangeIsFull) {
  io().maxBytesPerRead = 1000;
  expectLoad(0, 8192);
  EXPECT_EQ(io().readCalls, 9);
  EXPECT_EQ(io().actualBytes, 8192u);
}

TEST_F(TxtPaginationWindow, SeekFailureIsReportedAndRecoverable) {
  expectLoad(0, 100);
  io().failSeek = true;
  char dummy = 0;
  char* text = &dummy;
  EXPECT_EQ(window.load(5000, 100, text), ReadStatus::SeekFailed);
  EXPECT_EQ(text, nullptr);
  io().failSeek = false;
  expectLoad(5000, 100);
  EXPECT_EQ(io().openCount, 2);
}

TEST_F(TxtPaginationWindow, OpenFailureIsReportedAndRecoverable) {
  io().failOpen = true;
  char dummy = 0;
  char* text = &dummy;
  EXPECT_EQ(window.load(0, 100, text), ReadStatus::OpenFailed);
  EXPECT_EQ(text, nullptr);
  io().failOpen = false;
  expectLoad(0, 100);
  EXPECT_EQ(io().openCount, 1);
}

TEST_F(TxtPaginationWindow, FailedPageLoadReportsItsStatusAndNeverRunsTheWalker) {
  io().truncateAt = 100;
  MeasureProbe measure;
  SinkProbe sink;
  const TxtPagination::PageLayout layout{100, 10, {&measure, probePrepare, probeWidth, nullptr}, false, 0};
  const TxtPagination::LineSink lineSink{&sink, countingSink};
  const TxtPagination::PageResult result = TxtPagination::loadPage(window, 0, data.size(), layout, &lineSink);
  EXPECT_EQ(result.status, ReadStatus::ShortRead);
  EXPECT_EQ(result.lineCount, 0);
  EXPECT_EQ(result.nextOffset, 0u);
  EXPECT_EQ(measure.prepares, 0);
  EXPECT_EQ(measure.widths, 0);
  EXPECT_EQ(sink.calls, 0);
}

TEST_F(TxtPaginationWindow, OffsetAtOrPastTheEndIsInvalidWithoutIo) {
  MeasureProbe measure;
  const TxtPagination::PageLayout layout{100, 10, {&measure, probePrepare, probeWidth, nullptr}, false, 0};
  for (const size_t offset : {data.size(), data.size() + 1}) {
    const TxtPagination::PageResult result = TxtPagination::loadPage(window, offset, data.size(), layout, nullptr);
    EXPECT_EQ(result.status, ReadStatus::InvalidRange);
    EXPECT_EQ(result.lineCount, 0);
    EXPECT_EQ(result.nextOffset, offset);
  }
  EXPECT_EQ(io().openCount, 0);
  EXPECT_EQ(measure.prepares, 0);
}

// ---------------------------------------------------------------------------------------------
// Earlier firmware: Latin text paginates exactly as the frozen legacy loader did.
// ---------------------------------------------------------------------------------------------

namespace {

// ASCII words, runs of spaces, blank lines and CRLF: the text the legacy loader handled correctly.
std::string asciiText(const uint32_t seed, const size_t targetBytes) {
  static const char* const kWords[] = {"a",   "I",    "to",        "the",  "quick", "brown",           "fox",
                                       "of",  "over", "lazy",      "dogs", "1234",  "extraordinarily", "x",
                                       "and", "it",   "wonderful", "rain", "Total", "supercalifragilisticexpialidocious"};
  static const char* const kSeparators[] = {" ", " ", " ", " ", "  ", "   ", "\n", "\n", "\r\n", "\n\n"};
  Lcg rng(seed);
  std::string out;
  out.reserve(targetBytes + 64);
  while (out.size() < targetBytes) {
    out += kWords[rng.below(static_cast<uint32_t>(std::size(kWords)))];
    out += kSeparators[rng.below(static_cast<uint32_t>(std::size(kSeparators)))];
  }
  return out;
}

void expectLegacyPages(const std::string& label, const std::string& bytes, const Config& config) {
  SCOPED_TRACE(label + " " + describeConfig(config));
  const Sweep legacy = sweepLegacy(bytes, config);
  const Sweep production = sweepNew(bytes, config, true);
  ASSERT_TRUE(legacy.completed);
  EXPECT_EQ(diffSweeps(legacy, production, true), "");
}

}  // namespace

TEST(TxtPaginationLegacy, NotesFixtureMatchesTheLegacyLoaderOnEveryPage) {
  const std::string notes = notesFixture();
  ASSERT_EQ(notes.size(), 165998u);
  expectLegacyPages("notes fixture", notes, {440, 26, Model::Simple});
  expectLegacyPages("notes fixture", notes, {300, 40, Model::Simple});
  expectLegacyPages("notes fixture", notes, {40, 9, Model::Simple});
}

TEST(TxtPaginationLegacy, AsciiTextMatchesTheLegacyLoader) {
  const std::vector<Config> configs = {{440, 26, Model::Simple}, {300, 40, Model::Simple}, {120, 17, Model::Simple},
                                       {40, 9, Model::Simple},   {12, 5, Model::Simple},   {480, 1, Model::Simple}};
  for (uint32_t seed = 1; seed <= 24; seed++) {
    const std::string bytes = asciiText(seed, 3000 + (seed * 541) % 15000);
    expectLegacyPages("ascii seed " + std::to_string(seed), bytes, configs[seed % configs.size()]);
  }
}

// ---------------------------------------------------------------------------------------------
// Retained window replay and failures
// ---------------------------------------------------------------------------------------------

TEST(TxtPaginationReplay, RetainedWindowMatchesFreshLoadsForAnyOffsetOrder) {
  struct Subject {
    std::string bytes;
    Config config;
  };
  std::vector<Subject> subjects;
  subjects.push_back(Subject{notesFixture(), Config{440, 26, Model::Complex}});
  for (uint32_t seed = 101; seed < 107; seed++) {
    subjects.push_back(Subject{randomText(seed, 15000, seed % 3 == 0), Config{330, 12, Model::Complex}});
    subjects.push_back(Subject{cjkText(seed, 15000, 500), cjkStyle({330, 12, Model::Simple}, 40)});
  }

  for (const Subject& subject : subjects) {
    const size_t size = subject.bytes.size();
    const Sweep sweep = sweepNew(subject.bytes, subject.config, false);
    ASSERT_FALSE(sweep.pages.empty());

    std::vector<Access> accesses;
    for (size_t i = 0; i < sweep.pages.size() && i < 40; i++) accesses.push_back({sweep.pages[i].start, false});
    accesses.push_back({sweep.pages[0].start, false});  // backward to the start, then a repeat
    accesses.push_back({sweep.pages[0].start, false});
    accesses.push_back({sweep.pages.back().start, false});                  // distant jump
    accesses.push_back({sweep.pages[sweep.pages.size() / 2].start, true});  // reopen first
    Lcg rng(static_cast<uint64_t>(size));
    for (int i = 0; i < 60; i++) {
      accesses.push_back({rng.below(static_cast<uint32_t>(size + 1)), rng.below(10) == 0});
    }
    accesses.push_back({size, false});
    accesses.push_back({size + 7, false});
    EXPECT_EQ(diffAccessSequence(subject.bytes, subject.config, accesses), "");
  }
}

TEST(TxtPaginationReplay, ReadErrorMidSweepStopsTheSweepIncomplete) {
  const std::string notes = notesFixture();
  const Config config{440, 26, Model::Complex};

  Faults error;
  error.errorOnReadCall = 5;  // one read per page: page 5 fails
  const Sweep failed = sweepNew(notes, config, false, &error);
  EXPECT_FALSE(failed.completed);
  ASSERT_EQ(failed.pages.size(), 5u);
  for (size_t i = 0; i < 4; i++) EXPECT_TRUE(failed.pages[i].ok);
  EXPECT_FALSE(failed.pages[4].ok);
  EXPECT_EQ(failed.pages[4].status, ReadStatus::ReadError);
  EXPECT_EQ(failed.pages[4].next, failed.pages[4].start);

  Faults truncated;
  truncated.truncateAt = 50000;  // the source ends earlier than the recorded size
  const Sweep shortSweep = sweepNew(notes, config, false, &truncated);
  EXPECT_FALSE(shortSweep.completed);
  ASSERT_FALSE(shortSweep.pages.empty());
  EXPECT_FALSE(shortSweep.pages.back().ok);
  EXPECT_EQ(shortSweep.pages.back().status, ReadStatus::ShortRead);
  EXPECT_LE(shortSweep.pages.back().start, 50000u);
}

// ---------------------------------------------------------------------------------------------
// Counters (printed for the report; bounds asserted)
// ---------------------------------------------------------------------------------------------

TEST(TxtPaginationCounters, ASweepUsesOneHandleAndReadsTheSourceOnce) {
  const std::string notes = notesFixture();
  const std::string han = cjkText(3, 300000, 600);
  const Config config{440, 26, Model::Complex};
  const Sweep legacy = sweepLegacy(notes, config);
  const Sweep counted = sweepNew(notes, config, false);
  const Sweep collected = sweepNew(notes, config, true);
  const Sweep cjk = sweepNew(han, cjkStyle(config, 40), false);
  printCounters("legacy notes 440x26", legacy);
  printCounters("production count-only", counted);
  printCounters("production collect", collected);
  printCounters("production cjk 300KB", cjk);

  // Legacy: one open, one seek and one 8KB read per page.
  EXPECT_EQ(legacy.counters.opens, static_cast<int>(legacy.pages.size()));
  EXPECT_EQ(legacy.counters.seeks, static_cast<int>(legacy.pages.size()));

  // Production: a single open and a forward-only pass that never exceeds the source plus one window.
  struct Checked {
    const Sweep* sweep;
    size_t size;
  };
  const Checked checks[] = {{&counted, notes.size()}, {&collected, notes.size()}, {&cjk, han.size()}};
  for (const auto& [sweep, size] : checks) {
    EXPECT_TRUE(sweep->completed);
    EXPECT_EQ(sweep->counters.opens, 1);
    EXPECT_EQ(sweep->counters.seeks, 0);
    EXPECT_EQ(sweep->counters.shortReads, 0);
    EXPECT_EQ(sweep->counters.errorReads, 0);
    EXPECT_EQ(sweep->counters.requestedBytes, sweep->counters.actualBytes);
    EXPECT_LE(sweep->counters.actualBytes, size);
  }
  EXPECT_LT(counted.counters.requestedBytes, legacy.counters.requestedBytes);
  EXPECT_EQ(counted.counters.widthCalls, collected.counters.widthCalls);
}

TEST(TxtPaginationCounters, FirstPageReloadAfterTheSweepSeeksOnceWithoutReopening) {
  const std::string notes = notesFixture();
  const Config config{440, 26, Model::Complex};
  installSource(notes);
  WidthOracle oracle(config.model);
  TxtPagination::SourceWindow window;
  window.setSource(kSourcePath);
  size_t offset = 0;
  while (offset < notes.size()) {
    const PageRecord page = loadNewPage(window, notes.size(), config, oracle, offset, false);
    ASSERT_TRUE(page.ok);
    offset = page.next;
  }

  const FakeSource& io = Storage.source();
  const int opensBefore = io.openCount;
  const int seeksBefore = io.seekCount;
  const size_t requestedBefore = io.requestedBytes;
  const PageRecord first = loadNewPage(window, notes.size(), config, oracle, 0, true);
  ASSERT_TRUE(first.ok);
  EXPECT_EQ(io.openCount, opensBefore);
  EXPECT_EQ(io.seekCount, seeksBefore + 1);
  EXPECT_EQ(io.requestedBytes - requestedBefore, TxtPagination::PAGE_INPUT_BYTES);

  const Sweep sweep = sweepNew(notes, config, true);
  EXPECT_EQ(first.lines, sweep.pages[0].lines);
}

TEST(TxtPaginationCounters, WalkerAndWindowDoNotAllocateAfterTheFirstPage) {
  const std::string notes = notesFixture();
  const std::string han = cjkText(4, 120000, 400);
  struct Subject {
    const std::string* bytes;
    Config config;
  };
  const Subject subjects[] = {{&notes, Config{440, 26, Model::Complex}},
                              {&han, cjkStyle({440, 26, Model::Simple}, 40)}};
  for (const auto& [bytes, config] : subjects) {
    SCOPED_TRACE(describeConfig(config));
    const size_t size = bytes->size();
    // With and without materializing lines; the first page may allocate the window buffer.
    size_t allocations[2] = {0, 0};
    uint64_t collectedHash = 0;
    size_t pageCount = 0;
    for (int mode = 0; mode < 2; mode++) {
      installSource(*bytes);
      WidthOracle oracle(config.model);
      NewAdapter adapter{&oracle, nullptr};
      TxtPagination::PageLayout layout = makeLayout(config, adapter);
      layout.measure.preparePage = nullptr;
      HashSink hash;
      const TxtPagination::LineSink sink{&hash, hashLine};
      const TxtPagination::LineSink* sinkArg = mode == 1 ? &sink : nullptr;
      TxtPagination::SourceWindow window;
      window.setSource(kSourcePath);

      size_t offset = 0;
      size_t pages = 0;
      TxtPagination::PageResult result = TxtPagination::loadPage(window, 0, size, layout, sinkArg);
      if (result.status == ReadStatus::Ok) {
        offset = result.nextOffset;
        pages = 1;
      }
      alloc_counter::start();
      while (offset < size) {
        result = TxtPagination::loadPage(window, offset, size, layout, sinkArg);
        if (result.status != ReadStatus::Ok || result.nextOffset <= offset) break;
        offset = result.nextOffset;
        pages++;
      }
      allocations[mode] = alloc_counter::stop();
      EXPECT_EQ(offset, size);
      if (mode == 1) {
        collectedHash = hash.hash;
        pageCount = pages;
      }
    }

    HashSink expectedHash;
    const Sweep sweep = sweepNew(*bytes, config, true);
    for (const PageRecord& page : sweep.pages) {
      for (const std::string& line : page.lines) foldLine(expectedHash, line.data(), line.size());
    }
    EXPECT_EQ(collectedHash, expectedHash.hash);
    EXPECT_EQ(pageCount, sweep.pages.size());
    EXPECT_EQ(allocations[0], 0u);
    EXPECT_EQ(allocations[1], 0u);
  }
}

// ---------------------------------------------------------------------------------------------
// Page index: whole-source sweep and the incremental builder
// ---------------------------------------------------------------------------------------------

namespace {

struct IndexBuild {
  TxtPagination::IndexResult result{IndexStatus::Ok, ReadStatus::Ok, 0};
  std::vector<uint32_t> offsets;
  std::vector<uint32_t> chapters;
  std::vector<size_t> progress;  // the page count handed to pageDone after each page
};

void recordProgress(void* ctx, const size_t pageCount) { static_cast<std::vector<size_t>*>(ctx)->push_back(pageCount); }

IndexBuild buildIndexNew(const std::string& bytes, const Config& config, const Faults* faults = nullptr) {
  installSource(bytes, faults);
  WidthOracle oracle(config.model);
  NewAdapter adapter{&oracle, nullptr};
  const TxtPagination::PageLayout layout = makeLayout(config, adapter);
  TxtPagination::SourceWindow window;
  window.setSource(kSourcePath);

  IndexBuild build;
  build.result = TxtPagination::buildPageIndex(window, bytes.size(), layout, build.offsets, &build.chapters,
                                               recordProgress, &build.progress);
  return build;
}

// The page starts a page-by-page sweep implies: page 0, then every later page that shows a line (blank lines
// alone, in the CJK paragraph style, join the following page or end the book).
std::vector<uint32_t> sweepPageStarts(const std::string& bytes, const Config& config) {
  const Sweep sweep = sweepNew(bytes, config, false);
  EXPECT_TRUE(sweep.completed);
  std::vector<uint32_t> starts{0};
  for (size_t i = 1; i < sweep.pages.size(); i++) {
    if (sweep.pages[i].lineCount > 0) starts.push_back(static_cast<uint32_t>(sweep.pages[i].start));
  }
  return starts;
}

// Offsets of every physical line that is a chapter heading, found independently of the page walker.
std::vector<uint32_t> headingLines(const std::string& bytes) {
  std::vector<uint32_t> out;
  size_t start = 0;
  while (start < bytes.size()) {
    const size_t newline = bytes.find('\n', start);
    const size_t end = newline == std::string::npos ? bytes.size() : newline;
    size_t length = end - start;
    if (length > 0 && bytes[end - 1] == '\r') length--;
    if (out.size() < TxtPagination::MAX_CHAPTERS && TxtChapters::isHeading(bytes.data() + start, length)) {
      out.push_back(static_cast<uint32_t>(start));
    }
    start = end + 1;
  }
  return out;
}

TxtPageIndexFormat::Key keyFor(const std::string& bytes, const Config& config) {
  return TxtPageIndexFormat::Key{static_cast<uint32_t>(bytes.size()),
                                 0x5157u,
                                 config.viewportWidth,
                                 config.linesPerPage,
                                 3,
                                 20,
                                 0,
                                 static_cast<uint8_t>(config.cjkParagraphs ? TxtPageIndexFormat::LAYOUT_CJK_PARAGRAPHS : 0),
                                 static_cast<int16_t>(config.firstLineIndent)};
}

// Builds the index a few pages at a time as the reader does between loop passes, persisting the partial
// index and resuming from the file at random points (sleep, reopening the book), and checks the
// invariants a partial index promises after every step.
void buildInSteps(const std::string& bytes, const Config& config, const uint32_t seed, IndexBuild& build) {
  installSource(bytes);
  WidthOracle oracle(config.model);
  NewAdapter adapter{&oracle, nullptr};
  const TxtPagination::PageLayout layout = makeLayout(config, adapter);
  const TxtPageIndexFormat::Key key = keyFor(bytes, config);
  auto window = std::make_unique<TxtPagination::SourceWindow>();
  window->setSource(kSourcePath);

  TxtPagination::PageIndexBuilder builder;
  builder.reset(bytes.size());
  Lcg rng(seed);
  while (!builder.complete()) {
    const size_t before = builder.frontier();
    const TxtPagination::IndexResult result = builder.step(*window, layout, 1 + rng.below(6));
    if (result.status != IndexStatus::Ok) {
      build.result = result;
      return;
    }
    ASSERT_GT(builder.frontier(), before);
    ASSERT_EQ(result.offset, builder.frontier());
    ASSERT_EQ(TxtPageIndexFormat::checkOffsets(builder.offsets(), bytes.size()), TxtPageIndexFormat::Check::Ok);
    ASSERT_EQ(TxtPageIndexFormat::checkChapters(builder.chapters(), builder.offsets(), bytes.size(),
                                                builder.complete()),
              TxtPageIndexFormat::Check::Ok);
    if (builder.complete() || rng.below(4) != 0) continue;

    std::vector<uint8_t> image;
    const auto append = [&](const uint8_t* data, const size_t length) {
      image.insert(image.end(), data, data + length);
      return true;
    };
    ASSERT_TRUE(TxtPageIndexFormat::writeIndex(append, key, false, builder.offsets(), builder.chapters()));
    size_t cursor = 0;
    const auto read = [&](uint8_t* out, const size_t length) {
      if (cursor + length > image.size()) return false;
      std::memcpy(out, image.data() + cursor, length);
      cursor += length;
      return true;
    };
    TxtPageIndexFormat::Loaded loaded;
    ASSERT_EQ(TxtPageIndexFormat::readIndex(read, image.size(), key, loaded), TxtPageIndexFormat::Check::Ok);
    ASSERT_FALSE(loaded.complete);
    builder = TxtPagination::PageIndexBuilder{};
    builder.adopt(bytes.size(), std::move(loaded.offsets), std::move(loaded.chapters), loaded.complete);
    window = std::make_unique<TxtPagination::SourceWindow>();
    window->setSource(kSourcePath);
  }
  build.result = {IndexStatus::Ok, ReadStatus::Ok, builder.frontier()};
  build.offsets = builder.offsets();
  build.chapters = builder.chapters();
}

// A Chinese book with headings in every form the detector accepts, prose lines longer than a page window
// and sentences that mention chapters without being headings.
std::string chapterBook() {
  const std::string sentence = "山下的水是天的心，他一时说了下去。";
  const std::string longProse = "　　" + repeatText(sentence, 200);  // about 10KB: crosses page windows
  const std::string prose = "　　" + repeatText(sentence, 12);
  std::string book;
  book += "序章\n" + prose + "\n" + prose + "\n";
  book += "第一章 风起\n" + longProse + "\n";
  book += "他翻开了第一章，读到深夜。\n第三章就这么写完了。\n" + prose + "\n\n\n";
  book += "　　第二章\r\n" + prose + "\r\n" + longProse + "\r\n";
  book += "第十二回合\n第一部分\n" + prose + "\n";
  book += "第十二回 大闹天宫\n" + prose + "\n";
  book += "Chapter 3 was the dull one.\nChapter 3: The Return\n" + longProse + "\n";
  book += "番外篇\n" + prose + "\n尾声\n" + prose;
  return book;
}

}  // namespace

TEST(TxtPageIndexBuild, OffsetsAreThePagesOfASweep) {
  const std::string notes = notesFixture();
  const std::vector<std::pair<std::string, std::string>> books = {
      {"notes", notes},
      {"chapters", chapterBook()},
      {"cjk paragraphs", cjkText(11, 60000, 900)},
      {"cjk single paragraph", cjkText(12, 50000, 0)},
      {"random", randomText(13, 40000, true)},
      {"blank tail", "甲\n" + std::string(9000, '\n')},
      {"blank head", std::string(9000, '\n') + "甲乙丙\n"},
  };
  const std::vector<Config> configs = {{440, 26, Model::Complex}, cjkStyle({440, 26, Model::Simple}, 40),
                                       {300, 40, Model::Simple}, {100000, 3, Model::Simple},
                                       cjkStyle({100000, 40, Model::Simple}, 40)};
  for (const auto& [name, bytes] : books) {
    for (const Config& config : configs) {
      SCOPED_TRACE(name + " " + describeConfig(config));
      const IndexBuild build = buildIndexNew(bytes, config);
      ASSERT_EQ(build.result.status, IndexStatus::Ok);
      EXPECT_EQ(build.result.read, ReadStatus::Ok);
      EXPECT_EQ(build.result.offset, bytes.size());
      EXPECT_EQ(build.offsets, sweepPageStarts(bytes, config));
      EXPECT_EQ(TxtPageIndexFormat::checkOffsets(build.offsets, bytes.size()), TxtPageIndexFormat::Check::Ok);
      EXPECT_EQ(build.chapters, headingLines(bytes));
      ASSERT_EQ(build.progress.size(), sweepNew(bytes, config, false).pages.size());
      EXPECT_EQ(build.progress.back(), build.offsets.size());
    }
  }
}

TEST(TxtPageIndexBuild, ChaptersAreTheHeadingLines) {
  const std::string book = chapterBook();
  std::vector<uint32_t> expected;
  for (const char* heading :
       {"序章\n", "第一章 风起\n", "　　第二章\r\n", "第十二回 大闹天宫\n", "Chapter 3: The Return\n", "番外篇\n", "尾声\n"}) {
    const size_t at = book.find(heading);
    ASSERT_NE(at, std::string::npos) << heading;
    expected.push_back(static_cast<uint32_t>(at));
  }
  ASSERT_EQ(headingLines(book), expected);
  for (const Config& config : {Config{440, 26, Model::Simple}, cjkStyle({440, 26, Model::Simple}, 40),
                               Config{100000, 3, Model::Simple}, cjkStyle({120, 40, Model::Simple}, 40)}) {
    SCOPED_TRACE(describeConfig(config));
    const IndexBuild build = buildIndexNew(book, config);
    ASSERT_EQ(build.result.status, IndexStatus::Ok);
    EXPECT_EQ(build.chapters, expected);
  }
}

TEST(TxtPageIndexBuild, ChapterListIsCapped) {
  std::string book;
  for (int i = 1; i <= 9000; i++) book += "第" + std::to_string(i) + "章\n";
  const IndexBuild build = buildIndexNew(book, {440, 26, Model::Simple});
  ASSERT_EQ(build.result.status, IndexStatus::Ok);
  ASSERT_EQ(build.chapters.size(), TxtPagination::MAX_CHAPTERS);
  const std::vector<uint32_t> all = headingLines(book);
  EXPECT_EQ(build.chapters, std::vector<uint32_t>(all.begin(), all.begin() + TxtPagination::MAX_CHAPTERS));
}

TEST(TxtPageIndexBuild, AnEmptySourceIsTheSinglePageAtZero) {
  const IndexBuild build = buildIndexNew("", {440, 26, Model::Complex});
  EXPECT_EQ(build.result.status, IndexStatus::Ok);
  EXPECT_EQ(build.offsets, std::vector<uint32_t>{0});
  EXPECT_TRUE(build.chapters.empty());
  EXPECT_TRUE(build.progress.empty());
  EXPECT_EQ(Storage.source().readCalls, 0);
}

TEST(TxtPageIndexBuild, ParagraphStyleFoldsBlankPages) {
  const Config config = cjkStyle({100, 1, Model::Simple}, 40);
  EXPECT_EQ(buildIndexNew("　　甲乙\n　　丙丁\n", config).offsets, (std::vector<uint32_t>{0, 13}));
  EXPECT_EQ(buildIndexNew("甲\n　　", config).offsets, std::vector<uint32_t>{0});
  EXPECT_EQ(buildIndexNew("甲\n\n\n\n乙", config).offsets, (std::vector<uint32_t>{0, 4}));
}

TEST(TxtPageIndexBuild, ProgressIsReportedOncePerPage) {
  const std::string notes = notesFixture();
  const IndexBuild build = buildIndexNew(notes, {440, 26, Model::Complex});
  ASSERT_EQ(build.result.status, IndexStatus::Ok);
  const size_t pages = build.offsets.size();
  ASSERT_EQ(build.progress.size(), pages);
  // Page i is done once its successor start is recorded; the last page has no successor.
  for (size_t i = 0; i < pages; i++) EXPECT_EQ(build.progress[i], std::min(i + 2, pages));
}

TEST(TxtPageIndexBuild, AReadFailureLeavesNoIndexAtAnyPoint) {
  const std::string notes = notesFixture();
  const Config config{440, 26, Model::Complex};
  const IndexBuild good = buildIndexNew(notes, config);
  ASSERT_EQ(good.result.status, IndexStatus::Ok);
  ASSERT_GT(good.offsets.size(), 60u);

  // One read per page: read call c is page c - 1 (zero based).
  for (const int call : {1, 2, 5, 60}) {
    SCOPED_TRACE("error on read call " + std::to_string(call));
    Faults error;
    error.errorOnReadCall = call;
    const IndexBuild failed = buildIndexNew(notes, config, &error);
    EXPECT_EQ(failed.result.status, IndexStatus::ReadFailed);
    EXPECT_EQ(failed.result.read, ReadStatus::ReadError);
    EXPECT_EQ(failed.result.offset, good.offsets[static_cast<size_t>(call - 1)]);
    EXPECT_TRUE(failed.offsets.empty());
    EXPECT_TRUE(failed.chapters.empty());
    EXPECT_EQ(failed.progress.size(), static_cast<size_t>(call - 1));
  }

  Faults zero;
  zero.zeroOnReadCall = 3;
  const IndexBuild stalled = buildIndexNew(notes, config, &zero);
  EXPECT_EQ(stalled.result.status, IndexStatus::ReadFailed);
  EXPECT_EQ(stalled.result.read, ReadStatus::ShortRead);
  EXPECT_TRUE(stalled.offsets.empty());

  Faults truncated;
  truncated.truncateAt = 50000;  // the source ends before its recorded size
  const IndexBuild shortSource = buildIndexNew(notes, config, &truncated);
  EXPECT_EQ(shortSource.result.status, IndexStatus::ReadFailed);
  EXPECT_EQ(shortSource.result.read, ReadStatus::ShortRead);
  EXPECT_LE(shortSource.result.offset, 50000u);
  EXPECT_TRUE(shortSource.offsets.empty());
}

TEST(TxtPageIndexBuild, AStalledLayoutIsNotAnIndex) {
  // No lines fit on a page, so the sweep can never leave offset 0.
  const IndexBuild build = buildIndexNew(notesFixture(), {440, 0, Model::Complex});
  EXPECT_EQ(build.result.status, IndexStatus::NoProgress);
  EXPECT_EQ(build.result.read, ReadStatus::Ok);
  EXPECT_EQ(build.result.offset, 0u);
  EXPECT_TRUE(build.offsets.empty());
  EXPECT_TRUE(build.progress.empty());
}

TEST(TxtPageIndexBuilder, SteppingWithSavesAndResumesEqualsTheFullBuild) {
  const std::vector<std::pair<std::string, std::string>> books = {
      {"notes", notesFixture()},
      {"chapters", chapterBook()},
      {"cjk paragraphs", cjkText(21, 80000, 700)},
      {"random", randomText(22, 30000, true)},
      {"blank tail", "甲\n" + std::string(9000, '\n')},
      {"empty", ""},
      {"one byte", "a"},
  };
  const std::vector<Config> configs = {{440, 26, Model::Complex}, cjkStyle({440, 26, Model::Simple}, 40),
                                       {100000, 3, Model::Simple}};
  uint32_t seed = 1;
  for (const auto& [name, bytes] : books) {
    for (const Config& config : configs) {
      SCOPED_TRACE(name + " " + describeConfig(config));
      const IndexBuild full = buildIndexNew(bytes, config);
      ASSERT_EQ(full.result.status, IndexStatus::Ok);
      IndexBuild stepped;
      buildInSteps(bytes, config, seed++, stepped);
      if (HasFatalFailure()) return;
      EXPECT_EQ(stepped.result.status, IndexStatus::Ok);
      EXPECT_EQ(stepped.offsets, full.offsets);
      EXPECT_EQ(stepped.chapters, full.chapters);
    }
  }
}

TEST(TxtPageIndexBuilder, KnownPagesAreFinalBeforeTheIndexCompletes) {
  const std::string book = chapterBook();
  const Config config = cjkStyle({440, 26, Model::Simple}, 40);
  const IndexBuild full = buildIndexNew(book, config);
  ASSERT_EQ(full.result.status, IndexStatus::Ok);

  installSource(book);
  WidthOracle oracle(config.model);
  NewAdapter adapter{&oracle, nullptr};
  const TxtPagination::PageLayout layout = makeLayout(config, adapter);
  TxtPagination::SourceWindow window;
  window.setSource(kSourcePath);
  TxtPagination::PageIndexBuilder builder;
  builder.reset(book.size());
  EXPECT_TRUE(builder.covers(0));
  EXPECT_FALSE(builder.covers(1));
  while (!builder.complete()) {
    ASSERT_EQ(builder.step(window, layout, 2).status, IndexStatus::Ok);
    const size_t frontier = builder.frontier();
    for (size_t probe = 0; probe < book.size(); probe += 997) {
      if (probe < frontier) {
        ASSERT_TRUE(builder.covers(probe));
        ASSERT_EQ(builder.pageContaining(probe), TxtPagination::pageContaining(full.offsets, probe)) << probe;
      } else if (probe > frontier) {
        ASSERT_FALSE(builder.covers(probe));
      }
    }
    // Chapters found so far are the first chapters of the book.
    ASSERT_LE(builder.chapters().size(), full.chapters.size());
    ASSERT_TRUE(std::equal(builder.chapters().begin(), builder.chapters().end(), full.chapters.begin()));
  }
  EXPECT_TRUE(builder.covers(book.size() - 1));
  EXPECT_EQ(builder.offsets(), full.offsets);
}

TEST(TxtPageIndexBuilder, PageSelectionBeyondTheFrontierIndexesToTheRequestedPage) {
  const std::string book = chapterBook();
  const Config config = cjkStyle({440, 26, Model::Simple}, 40);
  const IndexBuild full = buildIndexNew(book, config);
  ASSERT_EQ(full.result.status, IndexStatus::Ok);
  ASSERT_GT(full.offsets.size(), 8u);
  const size_t target = full.offsets.size() / 2;

  installSource(book);
  WidthOracle oracle(config.model);
  NewAdapter adapter{&oracle, nullptr};
  const TxtPagination::PageLayout layout = makeLayout(config, adapter);
  TxtPagination::SourceWindow window;
  window.setSource(kSourcePath);
  TxtPagination::PageIndexBuilder builder;
  builder.reset(book.size());
  ASSERT_EQ(builder.step(window, layout, 1).status, IndexStatus::Ok);
  ASSERT_LE(builder.offsets().size(), target);

  // A page request waits for its start offset, not for the whole book.
  while (!builder.complete() && builder.offsets().size() <= target) {
    ASSERT_EQ(builder.step(window, layout, 2).status, IndexStatus::Ok);
  }
  ASSERT_FALSE(builder.complete());
  ASSERT_GT(builder.offsets().size(), target);
  EXPECT_EQ(builder.offsets()[target], full.offsets[target]);
  const PageRecord selected = loadNewPage(window, book.size(), config, oracle, builder.offsets()[target], true);
  ASSERT_TRUE(selected.ok);
  EXPECT_EQ(selected.next, full.offsets[target + 1]);
}

TEST(TxtPageIndexBuilder, AnOverestimatedPageRequestReachesTheActualLastPage) {
  const std::string book = chapterBook();
  const Config config = cjkStyle({440, 26, Model::Simple}, 40);
  const IndexBuild full = buildIndexNew(book, config);
  ASSERT_EQ(full.result.status, IndexStatus::Ok);
  const size_t target = full.offsets.size() + 100;

  installSource(book);
  WidthOracle oracle(config.model);
  NewAdapter adapter{&oracle, nullptr};
  const TxtPagination::PageLayout layout = makeLayout(config, adapter);
  TxtPagination::SourceWindow window;
  window.setSource(kSourcePath);
  TxtPagination::PageIndexBuilder builder;
  builder.reset(book.size());
  while (!builder.complete() && builder.offsets().size() <= target) {
    ASSERT_EQ(builder.step(window, layout, 2).status, IndexStatus::Ok);
  }
  ASSERT_TRUE(builder.complete());
  ASSERT_EQ(builder.offsets(), full.offsets);
  const size_t selectedPage = std::min(target, builder.offsets().size() - 1);
  EXPECT_EQ(builder.offsets()[selectedPage], full.offsets.back());
  const PageRecord selected = loadNewPage(window, book.size(), config, oracle, builder.offsets()[selectedPage], true);
  ASSERT_TRUE(selected.ok);
  EXPECT_GT(selected.lineCount, 0);
  EXPECT_EQ(selected.next, book.size());
}

TEST(TxtPageIndexBuilder, AFailedStepKeepsTheIndexAndCanBeRetried) {
  const std::string notes = notesFixture();
  const Config config{440, 26, Model::Complex};
  const IndexBuild full = buildIndexNew(notes, config);
  ASSERT_EQ(full.result.status, IndexStatus::Ok);

  installSource(notes);
  WidthOracle oracle(config.model);
  NewAdapter adapter{&oracle, nullptr};
  const TxtPagination::PageLayout layout = makeLayout(config, adapter);
  TxtPagination::SourceWindow window;
  window.setSource(kSourcePath);
  TxtPagination::PageIndexBuilder builder;
  builder.reset(notes.size());
  ASSERT_EQ(builder.step(window, layout, 10).status, IndexStatus::Ok);
  const std::vector<uint32_t> before = builder.offsets();
  ASSERT_EQ(before.size(), 11u);

  Storage.source().errorOnReadCall = Storage.source().readCalls + 1;
  const TxtPagination::IndexResult failed = builder.step(window, layout, 5);
  EXPECT_EQ(failed.status, IndexStatus::ReadFailed);
  EXPECT_EQ(failed.read, ReadStatus::ReadError);
  EXPECT_EQ(failed.offset, before.back());
  EXPECT_EQ(builder.offsets(), before);
  EXPECT_FALSE(builder.complete());

  Storage.source().errorOnReadCall = 0;
  while (!builder.complete()) ASSERT_EQ(builder.step(window, layout, 7).status, IndexStatus::Ok);
  EXPECT_EQ(builder.offsets(), full.offsets);
  EXPECT_EQ(builder.chapters(), full.chapters);
}

TEST(TxtPageIndexBuilder, AStalledLayoutMakesNoProgress) {
  const std::string book = chapterBook();
  installSource(book);
  WidthOracle oracle(Model::Simple);
  NewAdapter adapter{&oracle, nullptr};
  const TxtPagination::PageLayout layout = makeLayout({440, 0, Model::Simple}, adapter);
  TxtPagination::SourceWindow window;
  window.setSource(kSourcePath);
  TxtPagination::PageIndexBuilder builder;
  builder.reset(book.size());
  const TxtPagination::IndexResult result = builder.step(window, layout, 4);
  EXPECT_EQ(result.status, IndexStatus::NoProgress);
  EXPECT_EQ(result.offset, 0u);
  EXPECT_EQ(builder.offsets(), std::vector<uint32_t>{0});
  EXPECT_TRUE(builder.chapters().empty());
  EXPECT_FALSE(builder.complete());
}

TEST(TxtPageIndexBuilder, AnEmptySourceIsCompleteAtOnce) {
  TxtPagination::PageIndexBuilder builder;
  builder.reset(0);
  EXPECT_TRUE(builder.complete());
  EXPECT_EQ(builder.frontier(), 0u);
  EXPECT_EQ(builder.offsets(), std::vector<uint32_t>{0});
}

// ---------------------------------------------------------------------------------------------
// Reading position across firmware and layout changes
// ---------------------------------------------------------------------------------------------

TEST(TxtProgressMigration, AnEarlierFirmwarePageResumesAtTheSameText) {
  const std::string notes = notesFixture();
  const Config oldConfig{440, 26, Model::Simple};
  const Sweep legacy = sweepLegacy(notes, oldConfig);
  ASSERT_TRUE(legacy.completed);

  // The earlier firmware's index.bin (version 3) and progress.bin (u16 page).
  std::vector<uint8_t> image(TxtPageIndexFormat::LEGACY_HEADER_BYTES, 0);
  TxtPageIndexFormat::writeLe32(image.data(), TxtPageIndexFormat::FORMAT_MAGIC);
  image[4] = TxtPageIndexFormat::LEGACY_VERSION;
  TxtPageIndexFormat::writeLe32(image.data() + 5, static_cast<uint32_t>(notes.size()));
  TxtPageIndexFormat::writeLe32(image.data() + 26, static_cast<uint32_t>(legacy.pages.size()));
  for (const PageRecord& page : legacy.pages) {
    uint8_t word[4];
    TxtPageIndexFormat::writeLe32(word, static_cast<uint32_t>(page.start));
    image.insert(image.end(), word, word + 4);
  }
  const auto readAt = [&](const size_t at, uint8_t* out, const size_t length) {
    if (at + length > image.size()) return false;
    std::memcpy(out, image.data() + at, length);
    return true;
  };

  const std::vector<Config> newLayouts = {{300, 40, Model::Simple}, cjkStyle({440, 26, Model::Simple}, 40),
                                          {480, 12, Model::Complex}};
  for (const uint16_t page : {uint16_t{0}, uint16_t{1}, uint16_t{37}, static_cast<uint16_t>(legacy.pages.size() - 1)}) {
    SCOPED_TRACE("legacy page " + std::to_string(page));
    const uint8_t progress[4] = {static_cast<uint8_t>(page), static_cast<uint8_t>(page >> 8), 0, 0};
    const TxtProgressFormat::Position position = TxtProgressFormat::decode(progress, sizeof(progress));
    ASSERT_EQ(position.kind, TxtProgressFormat::Position::Kind::Page);
    ASSERT_EQ(position.page, page);

    uint32_t offset = 0;
    ASSERT_TRUE(TxtPageIndexFormat::legacyPageOffset(readAt, image.size(), static_cast<uint32_t>(notes.size()),
                                                     position.page, offset));
    EXPECT_EQ(offset, legacy.pages[page].start);

    for (const Config& layout : newLayouts) {
      SCOPED_TRACE(describeConfig(layout));
      const IndexBuild build = buildIndexNew(notes, layout);
      ASSERT_EQ(build.result.status, IndexStatus::Ok);
      // The new page holds the byte the old page started with.
      const size_t newPage = TxtPagination::pageContaining(build.offsets, offset);
      const size_t pageEnd = newPage + 1 < build.offsets.size() ? build.offsets[newPage + 1] : notes.size();
      EXPECT_LE(build.offsets[newPage], offset);
      EXPECT_LT(offset, pageEnd);

      // Saved again, the position is the byte offset, whatever layout reads it next.
      uint8_t saved[TxtProgressFormat::BYTES];
      TxtProgressFormat::encode(saved, static_cast<uint32_t>(newPage), offset,
                                TxtPageIndexFormat::keyFingerprint(keyFor(notes, layout)));
      const TxtProgressFormat::Position resumed = TxtProgressFormat::decode(saved, sizeof(saved));
      EXPECT_EQ(resumed.kind, TxtProgressFormat::Position::Kind::Offset);
      EXPECT_EQ(resumed.offset, offset);
      EXPECT_EQ(resumed.page, newPage);
    }
  }
}

TEST(TxtProgressMigration, APositionAheadOfAPartialIndexIsReachedByIndexing) {
  const std::string book = cjkText(31, 120000, 800);
  const Config config = cjkStyle({440, 26, Model::Simple}, 40);
  const IndexBuild full = buildIndexNew(book, config);
  ASSERT_EQ(full.result.status, IndexStatus::Ok);
  const uint32_t saved = full.offsets[full.offsets.size() * 3 / 4];

  installSource(book);
  WidthOracle oracle(config.model);
  NewAdapter adapter{&oracle, nullptr};
  const TxtPagination::PageLayout layout = makeLayout(config, adapter);
  TxtPagination::SourceWindow window;
  window.setSource(kSourcePath);
  TxtPagination::PageIndexBuilder builder;
  builder.reset(book.size());
  ASSERT_EQ(builder.step(window, layout, 3).status, IndexStatus::Ok);
  EXPECT_FALSE(builder.covers(saved));
  // The saved page itself lays out the same before the index reaches it.
  const PageRecord ahead = loadNewPage(window, book.size(), config, oracle, saved, true);
  ASSERT_TRUE(ahead.ok);
  const size_t page = TxtPagination::pageContaining(full.offsets, saved);
  EXPECT_EQ(ahead.next, page + 1 < full.offsets.size() ? full.offsets[page + 1] : book.size());

  while (!builder.covers(saved)) ASSERT_EQ(builder.step(window, layout, 5).status, IndexStatus::Ok);
  EXPECT_EQ(builder.pageContaining(saved), page);
  EXPECT_EQ(builder.offsets()[builder.pageContaining(saved)], saved);
}
