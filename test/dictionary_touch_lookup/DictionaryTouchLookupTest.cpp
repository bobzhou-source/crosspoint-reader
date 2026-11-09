// Press-and-hold dictionary lookup: the hit test on laid-out page words, the
// lookup key it produces against a real StarDict fixture, and the rule that
// decides whether a hold looks up, says no dictionary is set, or leaves the
// contact to the page-turn zones.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "DictZip.h"
#include "Dictionary.h"
#include "DictionaryRegistry.h"
#include "DictionaryRouter.h"
#include "StringUtils.h"
#include "src/activities/reader/DictionaryLookupSections.h"
#include "src/activities/reader/DictionaryPageWords.h"
#include "src/activities/reader/ReaderTouchZones.h"

namespace DictionaryRegistry {
bool resolveBasePath(const char* path, std::string& out) {
  out = path;
  return true;
}
}  // namespace DictionaryRegistry
namespace DictZip {
bool extractEntry(const char*, uint32_t, uint32_t, HalFile&, ExtractError*) { std::abort(); }
}  // namespace DictZip

namespace {

using DictionaryPageWords::WordBox;
using ReaderTouchZones::HoldAction;
using ReaderTouchZones::Layout;
using ReaderTouchZones::Zone;

constexpr int LINE_HEIGHT = 30;
constexpr int LINE_STEP = 36;  // line height plus leading

// Fixed-pitch stand-in for the renderer: 10 px per ASCII byte, 24 px per
// other character. The range form measures the non-NUL-terminated line
// prefixes the TXT extractor walks.
int advanceRange(const char* text, const size_t length) {
  int width = 0;
  for (size_t i = 0; i < length;) {
    uint32_t cp;
    const size_t n = CjkDictionary::characterBytes(text + i, cp);
    if (i + n > length) break;
    width += cp < 0x80 ? 10 : 24;
    i += n;
  }
  return width;
}

int advance(const char* text, EpdFontFamily::Style) { return advanceRange(text, strlen(text)); }

// A TXT page: display lines with their draw positions. Pointers into `texts`
// stay valid because the vector is complete before extraction starts.
class TxtLines {
 public:
  TxtLines(std::vector<std::string> lines, const int x, const int y, const int lineStep = LINE_STEP)
      : texts(std::move(lines)), lineHeight(LINE_HEIGHT) {
    layout.reserve(texts.size());
    for (size_t i = 0; i < texts.size(); ++i) {
      layout.push_back({texts[i].c_str(), x, y + static_cast<int>(i) * lineStep, EpdFontFamily::REGULAR});
    }
    DictionaryPageWords::extractFromLines(layout.data(), layout.size(), storage, words, rowCount,
                                          [](const char* text, const size_t length) {
                                            return advanceRange(text, length);
                                          });
  }

  int hold(const int x, const int y) const { return DictionaryPageWords::wordNear(words, x, y, lineHeight); }
  std::string textAt(const int x, const int y) const {
    const int hit = hold(x, y);
    if (hit < 0) return "";
    if (!words[hit].cjkBytes) return words[hit].text;
    char run[CjkDictionary::RUN_BYTES];
    DictionaryPageWords::cjkRun(words, hit, run);
    return run;
  }

  std::vector<std::string> texts;
  std::vector<DictionaryPageWords::LineText> layout;
  std::vector<std::string> storage;
  std::vector<WordBox> words;
  uint16_t rowCount = 0;
  int lineHeight;
};

// One laid-out line: tokens in order, separated by `gap` px, drawn the way
// DictionaryPageWords::extract() places them (line position plus margins).
struct Line {
  std::vector<std::string> tokens;
  int xPos = 0;
  int yPos = 0;
};

// A page built from lines the way the reader lays it out. Token storage is
// stable (reserved) because boxes point into it.
class FakePage {
 public:
  FakePage(std::vector<Line> lines, const int marginLeft, const int marginTop, const int gap = 10)
      : lines(std::move(lines)) {
    words.reserve(64);
    uint16_t run = 0;
    for (const auto& line : this->lines) {
      ++run;
      bool rowHasWords = false;
      int x = 0;
      for (const auto& token : line.tokens) {
        rowHasWords |= DictionaryPageWords::appendToken(words, token.c_str(), line.xPos + x + marginLeft,
                                                        line.yPos + marginTop, rowCount, EpdFontFamily::REGULAR, run);
        x += advance(token.c_str(), EpdFontFamily::REGULAR) + gap;
      }
      if (rowHasWords) rowCount++;
    }
    size_t index = 0;
    for (const auto& line : this->lines) {
      for (const auto& token : line.tokens) {
        DictionaryPageWords::measureToken(words, index, token.c_str(), EpdFontFamily::REGULAR, advance);
      }
    }
  }

  int hold(const int x, const int y) const { return DictionaryPageWords::wordNear(words, x, y, LINE_HEIGHT); }
  std::string textAt(const int x, const int y) const {
    const int hit = hold(x, y);
    if (hit < 0) return "";
    if (!words[hit].cjkBytes) return words[hit].text;
    char run[CjkDictionary::RUN_BYTES];
    DictionaryPageWords::cjkRun(words, hit, run);
    return run;
  }

  std::vector<Line> lines;
  std::vector<WordBox> words;
  uint16_t rowCount = 0;
};

// --- Latin word boundaries -------------------------------------------------

TEST(DictionaryHoldHitTest, LatinTokensKeepPunctuationAndHyphensForTheLookup) {
  // "Hello," at 100..159, "well-known" at 170..269, "—" at 280..303,
  // "(quoted)." at 314..403 (margin 100, top 50).
  const FakePage page({{{"Hello,", "well-known", "\xE2\x80\x94", "(quoted)."}, 0, 0}}, 100, 50);
  ASSERT_EQ(page.words.size(), 3u);  // the em dash alone is not a word
  EXPECT_EQ(page.textAt(105, 60), "Hello,");
  EXPECT_EQ(page.textAt(158, 60), "Hello,");      // on the comma
  EXPECT_EQ(page.textAt(215, 60), "well-known");  // on the hyphen
  EXPECT_EQ(page.textAt(268, 60), "well-known");
  EXPECT_EQ(page.textAt(400, 60), "(quoted).");

  EXPECT_EQ(Dictionary::cleanWord("Hello,"), "hello");
  EXPECT_EQ(Dictionary::cleanWord("well-known"), "well-known");
  EXPECT_EQ(Dictionary::cleanWord("(quoted)."), "quoted");
}

TEST(DictionaryHoldHitTest, GapBetweenWordsGoesToTheNearerWord) {
  const FakePage page({{{"Hello,", "well-known"}, 0, 0}}, 100, 50);
  EXPECT_EQ(page.textAt(162, 60), "Hello,");      // 3 px past "Hello,"
  EXPECT_EQ(page.textAt(167, 60), "well-known");  // 3 px before "well-known"
}

TEST(DictionaryHoldHitTest, BlankSpaceFindsNothing) {
  // Two paragraphs; the second starts 2 lines down (a blank line between).
  const FakePage page({{{"short", "line"}, 0, 0}, {{"next", "paragraph"}, 0, 2 * LINE_STEP}}, 100, 50);
  EXPECT_EQ(page.hold(100 + 50 + 10 + 40 + 9, 60), -1);  // 9 px past the end of "line"
  // "line" covers x 160..199.
  EXPECT_EQ(page.textAt(199 + DictionaryPageWords::HOLD_REACH, 60), "line");
  EXPECT_EQ(page.hold(199 + DictionaryPageWords::HOLD_REACH + 1, 60), -1);
  EXPECT_EQ(page.hold(100, 50 + LINE_HEIGHT + 12), -1);  // the blank line between paragraphs
  EXPECT_EQ(page.hold(100, 30), -1);                     // top margin, 20 px above the text
  EXPECT_EQ(page.textAt(100, 50 + 2 * LINE_STEP + 5), "next");
}

// --- Margins and orientation ----------------------------------------------

TEST(DictionaryHoldHitTest, MarginsShiftTheBoxesInEachOrientation) {
  // Portrait (480 wide) and landscape (800 wide) margins from
  // getOrientedViewableTRBL + readerMargin; logical coordinates come from
  // GfxRenderer::tapToLogical, so the hit test itself is orientation-free.
  struct Case {
    int marginLeft;
    int marginTop;
  };
  for (const Case c : {Case{18, 40}, Case{44, 12}}) {
    const FakePage page({{{"margin", "test"}, 0, 0}}, c.marginLeft, c.marginTop);
    EXPECT_EQ(page.textAt(c.marginLeft, c.marginTop), "margin");
    EXPECT_EQ(page.textAt(c.marginLeft - DictionaryPageWords::HOLD_REACH, c.marginTop + 10), "margin");
    EXPECT_EQ(page.hold(c.marginLeft - DictionaryPageWords::HOLD_REACH - 1, c.marginTop + 10), -1);
    EXPECT_EQ(page.hold(c.marginLeft + 5, c.marginTop - DictionaryPageWords::HOLD_REACH - 1), -1);
  }
}

// --- CJK --------------------------------------------------------------------

TEST(DictionaryHoldHitTest, CjkHoldStartsTheRunAtTheHeldCharacter) {
  // 系统，很好 as one layout token: the fullwidth comma ends the run.
  const FakePage page({{{"\xE7\xB3\xBB\xE7\xBB\x9F\xEF\xBC\x8C\xE5\xBE\x88\xE5\xA5\xBD"}, 0, 0}}, 20, 40);
  ASSERT_EQ(page.words.size(), 4u);
  EXPECT_EQ(page.words[0].x, 20);
  EXPECT_EQ(page.words[1].x, 44);
  EXPECT_EQ(page.words[2].x, 92);                              // after the comma's 24 px
  EXPECT_EQ(page.textAt(30, 50), "\xE7\xB3\xBB\xE7\xBB\x9F");  // on 系: 系统
  EXPECT_EQ(page.textAt(50, 50), "\xE7\xBB\x9F");              // on 统: 统 only
  EXPECT_EQ(page.textAt(95, 50), "\xE5\xBE\x88\xE5\xA5\xBD");  // on 很: 很好
}

TEST(DictionaryHoldHitTest, CjkRunStopsAtTheLineEndAndAtEightCharacters) {
  std::string ten;
  for (int i = 0; i < 10; ++i) ten += "\xE4\xB8\x80";  // 一 x10
  const FakePage page({{{ten}, 0, 0}, {{"\xE4\xBA\x8C"}, 0, LINE_STEP}}, 0, 0);
  std::string eight;
  for (int i = 0; i < 8; ++i) eight += "\xE4\xB8\x80";
  EXPECT_EQ(page.textAt(1, 5), eight);
  EXPECT_EQ(page.textAt(9 * 24 + 1, 5), "\xE4\xB8\x80");  // last on its line: the next line is a new run
  EXPECT_EQ(page.textAt(1, LINE_STEP + 5), "\xE4\xBA\x8C");
}

// --- TXT display lines ------------------------------------------------------

TEST(TxtWordExtraction, LatinWordsAreSplitAtSpacesWithPrefixWidths) {
  // "Hello, well-known world" at (100, 50): 10 px per ASCII byte.
  const TxtLines page({"Hello, well-known world"}, 100, 50);
  ASSERT_EQ(page.words.size(), 3u);
  EXPECT_EQ(page.words[0].x, 100);
  EXPECT_EQ(page.words[1].x, 170);  // 60 px word + 10 px space
  EXPECT_EQ(page.words[2].x, 280);  // another 100 px word + 10 px space
  EXPECT_EQ(page.textAt(105, 60), "Hello,");
  EXPECT_EQ(page.textAt(215, 60), "well-known");
  EXPECT_EQ(page.textAt(285, 60), "world");
  EXPECT_EQ(Dictionary::cleanWord("Hello,"), "hello");
}

TEST(TxtWordExtraction, CjkRunStartsAtTheHeldCharacterAndStopsAtPunctuation) {
  // 系统，很好 with 24 px per CJK codepoint.
  const TxtLines page({"\xE7\xB3\xBB\xE7\xBB\x9F\xEF\xBC\x8C\xE5\xBE\x88\xE5\xA5\xBD"}, 20, 40);
  ASSERT_EQ(page.words.size(), 4u);
  EXPECT_EQ(page.words[0].x, 20);
  EXPECT_EQ(page.words[1].x, 44);
  EXPECT_EQ(page.words[2].x, 92);                              // after the comma's 24 px
  EXPECT_EQ(page.textAt(30, 50), "\xE7\xB3\xBB\xE7\xBB\x9F");  // 系 → 系统
  EXPECT_EQ(page.textAt(50, 50), "\xE7\xBB\x9F");              // 统 → 统
  EXPECT_EQ(page.textAt(95, 50), "\xE5\xBE\x88\xE5\xA5\xBD");  // 很 → 很好
}

TEST(TxtWordExtraction, CjkWordsSeparatedBySpacesNeverJoin) {
  // Two separate layout tokens on one line must not share a CJK run.
  const TxtLines page({"\xE4\xB8\xAD\xE6\x96\x87 \xE5\xBE\x88\xE5\xA5\xBD"}, 0, 0);  // 中文 很好
  ASSERT_EQ(page.words.size(), 4u);
  EXPECT_EQ(page.textAt(1, 5), "\xE4\xB8\xAD\xE6\x96\x87");  // 中文
  EXPECT_EQ(page.textAt(59, 5), "\xE5\xBE\x88\xE5\xA5\xBD");  // 很好 starts after 48 px + 10 px space
}

TEST(TxtWordExtraction, LatinRunsInsideCjkStaySelectable) {
  // 中文abc很好: script changes split the token, so the Latin run keeps its
  // own box and neither CJK run absorbs it.
  const TxtLines page({"\xE4\xB8\xAD\xE6\x96\x87" "abc" "\xE5\xBE\x88\xE5\xA5\xBD"}, 0, 0);
  ASSERT_EQ(page.words.size(), 5u);
  EXPECT_EQ(page.words[2].x, 48);                            // abc starts after 中文
  EXPECT_EQ(page.textAt(1, 5), "\xE4\xB8\xAD\xE6\x96\x87");  // 中文
  EXPECT_EQ(page.textAt(55, 5), "abc");
  EXPECT_EQ(page.textAt(80, 5), "\xE5\xBE\x88\xE5\xA5\xBD");  // 很 → 很好
  EXPECT_EQ(page.textAt(105, 5), "\xE5\xA5\xBD");              // 好 → 好
}

TEST(TxtWordExtraction, LinesKeepTheirRowAndVerticalSpacing) {
  // A blank display line still advances y; rows count only word-bearing lines.
  const TxtLines page({"first line", "", "third"}, 10, 20);
  EXPECT_EQ(page.rowCount, 2u);
  EXPECT_EQ(page.words[0].row, 0u);
  EXPECT_EQ(page.words[2].row, 1u);
  EXPECT_EQ(page.textAt(15, 20 + 2 * LINE_STEP + 5), "third");
  EXPECT_EQ(page.hold(15, 20 + LINE_STEP + 5), -1);  // the blank line
}

// --- Lookup against a real StarDict fixture --------------------------------

void be32(std::ostream& out, const uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) out.put(static_cast<char>(value >> shift));
}

// A minimal StarDict (.idx/.dict/.ifo) at `stem`, sorted like the real ones.
void writeStarDict(const std::string& stem, std::vector<std::pair<std::string, std::string>> entries) {
  std::sort(entries.begin(), entries.end(),
            [](auto& a, auto& b) { return StringUtils::asciiCaseCmp(a.first.c_str(), b.first.c_str()) < 0; });
  std::ofstream idx(stem + ".idx", std::ios::binary);
  std::ofstream data(stem + ".dict", std::ios::binary);
  uint32_t offset = 0;
  for (const auto& [word, definition] : entries) {
    idx.write(word.c_str(), static_cast<std::streamsize>(word.size() + 1));
    be32(idx, offset);
    be32(idx, static_cast<uint32_t>(definition.size()));
    data << definition;
    offset += static_cast<uint32_t>(definition.size());
  }
  std::ofstream(stem + ".ifo") << "StarDict's dict ifo file\nversion=2.4.2\nsametypesequence=m\n";
}

std::filesystem::path freshDir(const char* prefix) {
  const auto dir = std::filesystem::temp_directory_path() /
                   (prefix + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  return dir;
}

// What DictionaryWordSelectActivity::performLookup does with words[hit] for
// one dictionary; returns "headword=definition".
std::string lookUpWords(Dictionary& dict, const std::vector<WordBox>& words, const int hit) {
  if (hit < 0) return "";
  char run[CjkDictionary::RUN_BYTES];
  char previous[CjkDictionary::RUN_BYTES];
  DictionaryPageWords::heldCjkRuns(words, hit, run, previous);
  std::string definition;
  std::string headword;
  uint8_t mask = 0;
  const bool found = run[0] ? dict.lookupCjk(run, definition, headword, mask, nullptr, previous)
                            : dict.lookup(words[hit].text, definition, headword);
  return found ? headword + "=" + definition : "not found";
}

class HoldLookup : public ::testing::Test {
 protected:
  void SetUp() override {
    dir = freshDir("hold-lookup-");
    const std::string stem = (dir / "fixture").string();
    writeStarDict(stem, {{"hello", "a greeting"},
                         {"well-known", "widely known"},
                         {"\xE7\xB3\xBB", "to tie"},
                         {"\xE7\xB3\xBB\xE7\xBB\x9F", "system"},
                         {"\xE7\xBB\x9F", "to unite"}});
    ASSERT_TRUE(dict.open(stem.c_str()));
    if (dict.needsIndex()) ASSERT_TRUE(dict.buildIndex());
  }
  void TearDown() override { std::filesystem::remove_all(dir); }

  std::string lookUp(const FakePage& page, const int x, const int y) {
    return lookUpWords(dict, page.words, page.hold(x, y));
  }
  std::string lookUpTxt(const TxtLines& page, const int x, const int y) {
    return lookUpWords(dict, page.words, page.hold(x, y));
  }

  std::filesystem::path dir;
  Dictionary dict;
};

TEST_F(HoldLookup, LatinHoldFindsTheCleanedWord) {
  const FakePage page({{{"Hello,", "well-known"}, 0, 0}}, 100, 50);
  EXPECT_EQ(lookUp(page, 158, 60), "hello=a greeting");
  EXPECT_EQ(lookUp(page, 215, 60), "well-known=widely known");
}

TEST_F(HoldLookup, CjkHoldFindsTheLongestMatchFromTheHeldCharacter) {
  const FakePage page({{{"\xE7\xB3\xBB\xE7\xBB\x9F\xE5\xBE\x88"}, 0, 0}}, 0, 0);  // 系统很
  EXPECT_EQ(lookUp(page, 5, 5), "\xE7\xB3\xBB\xE7\xBB\x9F=system");               // 系 → 系统
  EXPECT_EQ(lookUp(page, 30, 5), "\xE7\xB3\xBB\xE7\xBB\x9F=system");  // 统 → 系统 (the compound it ends)
  EXPECT_EQ(lookUp(page, 52, 5), "not found");                                    // 很 is not in the fixture
}

TEST_F(HoldLookup, TxtLatinHoldUsesTheDisplayLineWord) {
  const TxtLines page({"Hello, well-known world"}, 100, 50);
  EXPECT_EQ(lookUpTxt(page, 105, 60), "hello=a greeting");
  EXPECT_EQ(lookUpTxt(page, 215, 60), "well-known=widely known");
}

TEST_F(HoldLookup, TxtCjkHoldFindsTheLongestMatchFromTheHeldCharacter) {
  const TxtLines page({"\xE7\xB3\xBB\xE7\xBB\x9F\xE5\xBE\x88"}, 0, 0);  // 系统很
  EXPECT_EQ(lookUpTxt(page, 5, 5), "\xE7\xB3\xBB\xE7\xBB\x9F=system");  // 系 → 系统
  EXPECT_EQ(lookUpTxt(page, 30, 5), "\xE7\xB3\xBB\xE7\xBB\x9F=system");  // 统 → 系统 (the compound it ends)
  EXPECT_EQ(lookUpTxt(page, 52, 5), "not found");                       // 很 is not in the fixture
}

TEST(HeldCompound, SecondCharacterFindsTheCompoundItEnds) {
  const auto dir = freshDir("held-compound-");
  const std::string stem = (dir / "cedict").string();
  // 北京 Beijing, 京 capital, 北 north, 人 person; 京人 is not an entry.
  writeStarDict(stem, {{"\xE5\x8C\x97\xE4\xBA\xAC", "Beijing"},
                       {"\xE4\xBA\xAC", "capital"},
                       {"\xE5\x8C\x97", "north"},
                       {"\xE4\xBA\xBA", "person"}});
  Dictionary dict;
  ASSERT_TRUE(dict.open(stem.c_str()));
  ASSERT_TRUE(dict.buildIndex());
  const FakePage page({{{"\xE5\x8C\x97\xE4\xBA\xAC\xE4\xBA\xBA"}, 0, 0}}, 0, 0);  // 北京人
  EXPECT_EQ(lookUpWords(dict, page.words, page.hold(5, 5)), "\xE5\x8C\x97\xE4\xBA\xAC=Beijing");   // 北
  EXPECT_EQ(lookUpWords(dict, page.words, page.hold(30, 5)), "\xE5\x8C\x97\xE4\xBA\xAC=Beijing");  // 京
  EXPECT_EQ(lookUpWords(dict, page.words, page.hold(52, 5)), "\xE4\xBA\xBA=person");               // 人

  // The compound's match mask drops the lone first character (北), which
  // would not cover the held one; usedPrevious names the run it describes.
  char run[CjkDictionary::RUN_BYTES];
  char previous[CjkDictionary::RUN_BYTES];
  DictionaryPageWords::heldCjkRuns(page.words, page.hold(30, 5), run, previous);
  EXPECT_STREQ(previous, "\xE5\x8C\x97\xE4\xBA\xAC\xE4\xBA\xBA");
  std::string definition;
  std::string headword;
  uint8_t mask = 0;
  bool usedPrevious = false;
  ASSERT_TRUE(dict.lookupCjk(run, definition, headword, mask, nullptr, previous, &usedPrevious));
  EXPECT_TRUE(usedPrevious);
  EXPECT_EQ(mask, 2);

  // A held first character of a run has no previous run.
  DictionaryPageWords::heldCjkRuns(page.words, page.hold(5, 5), run, previous);
  EXPECT_STREQ(previous, "");
  std::filesystem::remove_all(dir);
}

TEST(CleanWord, CurlyApostrophesBecomeAscii) {
  EXPECT_EQ(Dictionary::cleanWord("don\xE2\x80\x99t"), "don't");
  EXPECT_EQ(Dictionary::cleanWord("Rock\xE2\x80\x98n"), "rock'n");
  EXPECT_EQ(Dictionary::cleanWord("\xE2\x80\x98quoted\xE2\x80\x99"), "quoted");
  EXPECT_EQ(Dictionary::cleanWord("Robert\xE2\x80\x99s,"), "robert's");
}

// --- Routing across installed dictionaries ------------------------------------

TEST(DictionaryRouting, OrderPutsTheSelectedThenScriptMatches) {
  const std::vector<std::string> installed{"cedict", "english", "jmdict", "zh-wiki"};
  std::vector<std::string> order;
  DictionaryRouter::order(installed, "cedict", false, order);
  EXPECT_EQ(order, (std::vector<std::string>{"cedict", "english", "jmdict", "zh-wiki"}));
  DictionaryRouter::order(installed, "english", true, order);
  EXPECT_EQ(order, (std::vector<std::string>{"english", "cedict", "zh-wiki", "jmdict"}));
  DictionaryRouter::order(installed, "", false, order);
  EXPECT_EQ(order, (std::vector<std::string>{"english", "cedict", "jmdict", "zh-wiki"}));
  EXPECT_TRUE(DictionaryRouter::suitsScript("en-gcide", false));
  EXPECT_TRUE(DictionaryRouter::suitsScript("CC-CEDICT", true));
  EXPECT_FALSE(DictionaryRouter::suitsScript("cedict", false));
}

class RoutedLookup : public ::testing::Test {
 protected:
  void SetUp() override {
    dir = freshDir("routed-lookup-");
    cedict = (dir / "cedict").string();
    english = (dir / "english").string();
    writeStarDict(cedict, {{"\xE5\x8C\x97\xE4\xBA\xAC", "Beijing"}, {"\xE4\xBA\xAC", "capital"}});
    writeStarDict(english, {{"hello", "a greeting"}, {"don't", "do not"}, {"dog", "canine"}});
  }
  void TearDown() override { std::filesystem::remove_all(dir); }

  std::string route(const std::vector<std::string>& names, const char* word,
                    DictionaryRouter::Miss* missOut = nullptr) {
    char run[CjkDictionary::RUN_BYTES] = {};
    uint32_t cp = 0;
    if (CjkDictionary::characterBytes(word, cp) && CjkDictionary::isCjk(cp)) {
      memcpy(run, word, std::min(strlen(word), sizeof(run) - 1));
    }
    std::string definition;
    std::string headword;
    uint8_t mask = 0;
    bool usedPrevious = false;
    DictionaryRouter::Miss miss = DictionaryRouter::Miss::NoDictionary;
    const bool found = router.lookup(names, word, run, "", [this] { ++indexBuilds; }, nullptr, definition, headword,
                                     mask, usedPrevious, miss);
    if (missOut) *missOut = miss;
    return found ? headword + "=" + definition : "not found";
  }

  std::filesystem::path dir;
  std::string cedict;
  std::string english;
  DictionaryRouter router;
  int indexBuilds = 0;
};

TEST_F(RoutedLookup, EnglishWordFallsThroughTheSelectedChineseDictionary) {
  EXPECT_EQ(route({cedict, english}, "Hello,"), "hello=a greeting");
  EXPECT_EQ(route({cedict, english}, "Don\xE2\x80\x99t"), "don't=do not");
  EXPECT_EQ(route({cedict, english}, "dogs"), "dog=canine");
  EXPECT_EQ(indexBuilds, 2);  // each dictionary indexed once, then kept
}

TEST_F(RoutedLookup, ChineseWordFallsThroughTheSelectedEnglishDictionary) {
  EXPECT_EQ(route({english, cedict}, "\xE5\x8C\x97\xE4\xBA\xAC"), "\xE5\x8C\x97\xE4\xBA\xAC=Beijing");
  EXPECT_TRUE(router.dictionary().isOpen());
}

TEST_F(RoutedLookup, MissesAndMissingFoldersAreReported) {
  DictionaryRouter::Miss miss;
  EXPECT_EQ(route({cedict, english}, "zebra", &miss), "not found");
  EXPECT_EQ(miss, DictionaryRouter::Miss::NotFound);
  // A selected folder that is gone does not hide the other dictionaries.
  EXPECT_EQ(route({(dir / "gone").string(), english}, "hello", &miss), "hello=a greeting");
  EXPECT_EQ(route({(dir / "gone").string()}, "hello", &miss), "not found");
  EXPECT_EQ(miss, DictionaryRouter::Miss::OpenFailed);
  EXPECT_EQ(route({}, "hello", &miss), "not found");
  EXPECT_EQ(miss, DictionaryRouter::Miss::NoDictionary);
}

// --- Who owns the hold ------------------------------------------------------

// detectTouchPageTurn's zones before they moved into ReaderTouchZones::zoneAt.
Zone legacyZone(const Layout& l, const int x, const int y, const int width, const int height) {
  if (!l.nextTaps && !l.prevTaps) return Zone::None;
  if (l.menuTap && x >= width / 3 && x < width - width / 3 && y >= height / 3 && y < height - height / 3) {
    return Zone::None;  // the menu tap, handled by isTouchMenuTap
  }
  const bool nextZone = l.inverted ? x < (width * 2) / 3 : x >= width / 3;
  const bool next = l.nextTaps && (!l.prevTaps || nextZone);
  const bool prev = l.prevTaps && (!l.nextTaps || !nextZone);
  EXPECT_NE(next, prev);
  return next ? Zone::Next : Zone::Prev;
}

TEST(ReaderTouchZonesTest, PageTurnTapsAreUnchanged) {
  for (int bits = 0; bits < 16; ++bits) {
    const Layout layout{(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0};
    for (const auto [width, height] : {std::pair{480, 800}, std::pair{800, 480}}) {
      for (int x = 0; x < width; x += 7) {
        for (int y = 0; y < height; y += 11) {
          Zone zone = ReaderTouchZones::zoneAt(layout, x, y, width, height);
          if (zone == Zone::Menu) zone = Zone::None;
          ASSERT_EQ(zone, legacyZone(layout, x, y, width, height)) << bits << " " << x << "," << y;
        }
      }
    }
  }
}

TEST(ReaderTouchZonesTest, ZonesSplitAtTheLeftThirdAndFlipWhenInverted) {
  const Layout both{true, true, false, true};
  EXPECT_EQ(ReaderTouchZones::zoneAt(both, 10, 10, 480, 800), Zone::Prev);
  EXPECT_EQ(ReaderTouchZones::zoneAt(both, 400, 10, 480, 800), Zone::Next);
  EXPECT_EQ(ReaderTouchZones::zoneAt(both, 240, 400, 480, 800), Zone::Menu);
  EXPECT_EQ(ReaderTouchZones::zoneAt(both, 400, 240, 800, 480), Zone::Menu);  // landscape centre
  const Layout inverted{true, true, true, true};
  EXPECT_EQ(ReaderTouchZones::zoneAt(inverted, 10, 10, 480, 800), Zone::Next);
  EXPECT_EQ(ReaderTouchZones::zoneAt(inverted, 400, 10, 480, 800), Zone::Prev);
  const Layout swipeOnly{false, false, false, true};
  EXPECT_EQ(ReaderTouchZones::zoneAt(swipeOnly, 10, 10, 480, 800), Zone::None);
}

TEST(ReaderTouchZonesTest, TapAndSwipeTurnsOnEitherSideWithoutStealingTheCenter) {
  // The default layout: both directions accept taps and the centre third is
  // the reader-menu target.
  const Layout layout{true, true, false, true};
  EXPECT_EQ(ReaderTouchZones::zoneAt(layout, 10, 10, 480, 800), Zone::Prev);
  EXPECT_EQ(ReaderTouchZones::zoneAt(layout, 470, 10, 480, 800), Zone::Next);
  EXPECT_EQ(ReaderTouchZones::zoneAt(layout, 240, 400, 480, 800), Zone::Menu);
  // Outside the centre band the sides still turn, even at mid-screen x.
  EXPECT_EQ(ReaderTouchZones::zoneAt(layout, 240, 10, 480, 800), Zone::Next);
  EXPECT_EQ(ReaderTouchZones::zoneAt(layout, 10, 400, 480, 800), Zone::Prev);
  // An RTL or inverted page swaps only the side meaning; the centre stays.
  const Layout inverted{true, true, true, true};
  EXPECT_EQ(ReaderTouchZones::zoneAt(inverted, 10, 10, 480, 800), Zone::Next);
  EXPECT_EQ(ReaderTouchZones::zoneAt(inverted, 470, 10, 480, 800), Zone::Prev);
  EXPECT_EQ(ReaderTouchZones::zoneAt(inverted, 240, 400, 480, 800), Zone::Menu);
}

TEST(ReaderTouchZonesTest, ConfiguredZoneHoldsKeepTheHold) {
  // Chapter skip / rotate on hold, tap zones on: the zone keeps its hold.
  EXPECT_EQ(ReaderTouchZones::holdAction(Zone::Next, true, true, true), HoldAction::None);
  EXPECT_EQ(ReaderTouchZones::holdAction(Zone::Prev, true, true, true), HoldAction::None);
  // The centre menu rectangle and swipe-only pages have no hold meaning.
  EXPECT_EQ(ReaderTouchZones::holdAction(Zone::Menu, true, true, true), HoldAction::Lookup);
  EXPECT_EQ(ReaderTouchZones::holdAction(Zone::None, true, true, true), HoldAction::Lookup);
  // Hold actions off (the default): a word in a tap zone looks up.
  EXPECT_EQ(ReaderTouchZones::holdAction(Zone::Next, false, true, true), HoldAction::Lookup);
  EXPECT_TRUE(ReaderTouchZones::zoneOwnsHold(Zone::Prev, true));
  EXPECT_FALSE(ReaderTouchZones::zoneOwnsHold(Zone::Prev, false));
  EXPECT_FALSE(ReaderTouchZones::zoneOwnsHold(Zone::Menu, true));
}

TEST(ReaderTouchZonesTest, BlankSpaceAndNoDictionary) {
  EXPECT_EQ(ReaderTouchZones::holdAction(Zone::None, false, false, true), HoldAction::None);
  EXPECT_EQ(ReaderTouchZones::holdAction(Zone::None, false, false, false), HoldAction::None);
  EXPECT_EQ(ReaderTouchZones::holdAction(Zone::None, false, true, false), HoldAction::NoDictionary);
  EXPECT_EQ(ReaderTouchZones::holdAction(Zone::Next, true, true, false), HoldAction::None);
}

// ---- Lookup popup sections: tabs, Compact / Longer, layout --------------------------------

using DictionaryLookup::LineKind;
using DictionaryLookup::LookupSections;
using DictionaryLookup::Section;

LookupSections fullSections() {
  LookupSections s;
  s.defShort = "1. a sense";
  s.defLong = "1. a sense\n2. another sense";
  s.storyShort = "Xiao Yan — clashed over the pact";
  s.storyLong = "Xiao Yan\n• Ch.1: [origin] introduced\n• Ch.3: [arc] clashed over the pact";
  s.relatedShort = "Moby Dick p.12 — “the whale…”";
  s.relatedLong = "• Moby Dick p.12\n“the whale breached beside the boat”";
  s.riffShort = "Winston weighs loyalty";
  s.riffLong = "Winston: weighs loyalty. In context: a cost. The tension: a choice.";
  return s;
}

TEST(DictionaryLookupSectionsTest, TabsCycleAndSkipEmptySections) {
  LookupSections s = fullSections();
  EXPECT_TRUE(s.hasExtras());
  EXPECT_EQ(DictionaryLookup::nextSection(s, Section::All, 1), Section::Def);
  EXPECT_EQ(DictionaryLookup::nextSection(s, Section::Riff, 1), Section::All);
  EXPECT_EQ(DictionaryLookup::nextSection(s, Section::All, -1), Section::Riff);
  EXPECT_EQ(DictionaryLookup::nextSection(s, Section::Def, -1), Section::All);

  s.storyShort.clear();
  s.storyLong.clear();
  s.relatedShort.clear();
  EXPECT_TRUE(s.has(Section::Related));  // the longer form alone still counts
  s.relatedLong.clear();
  EXPECT_EQ(DictionaryLookup::nextSection(s, Section::Def, 1), Section::Riff);
  EXPECT_EQ(DictionaryLookup::nextSection(s, Section::Riff, -1), Section::Def);
  EXPECT_TRUE(s.hasExtras());

  s.riffShort.clear();
  s.riffLong.clear();
  EXPECT_FALSE(s.hasExtras());  // a definition alone keeps the plain view
  s.defShort.clear();
  s.defLong.clear();
  EXPECT_EQ(DictionaryLookup::nextSection(s, Section::All, 1), Section::All);
  EXPECT_TRUE(s.has(Section::All));

  EXPECT_STREQ(DictionaryLookup::sectionLabel(Section::Related), "Related");
  EXPECT_STREQ(DictionaryLookup::sectionHeader(Section::Story), "STORY SO FAR");
}

TEST(DictionaryLookupSectionsTest, LongerFallsBackToCompactAndBack) {
  LookupSections s = fullSections();
  EXPECT_EQ(s.text(Section::Story, false), s.storyShort);
  EXPECT_EQ(s.text(Section::Story, true), s.storyLong);
  EXPECT_EQ(s.text(Section::All, true), "");
  s.riffLong.clear();
  EXPECT_EQ(s.text(Section::Riff, true), "Winston weighs loyalty");
  s.defShort.clear();
  EXPECT_EQ(s.text(Section::Def, false), s.defLong);
}

TEST(DictionaryLookupSectionsTest, ComposesHeadedSectionsWithOffsets) {
  LookupSections s = fullSections();
  s.relatedShort.clear();
  s.relatedLong.clear();
  std::vector<uint32_t> offsets = {99};
  const std::string all = DictionaryLookup::composeSections(s, Section::All, false, offsets);
  EXPECT_EQ(all,
            "DEFINITION\n1. a sense\n\nSTORY SO FAR\nXiao Yan — clashed over the pact\n\nMARGIN RIFF\nWinston weighs "
            "loyalty");
  ASSERT_EQ(offsets.size(), 3u);
  EXPECT_EQ(all.compare(offsets[0], 10, "DEFINITION"), 0);
  EXPECT_EQ(all.compare(offsets[1], 12, "STORY SO FAR"), 0);
  EXPECT_EQ(all.compare(offsets[2], 11, "MARGIN RIFF"), 0);

  const std::string story = DictionaryLookup::composeSections(s, Section::Story, true, offsets);
  EXPECT_EQ(story, "STORY SO FAR\n" + s.storyLong);
  EXPECT_EQ(offsets, std::vector<uint32_t>{0});
  EXPECT_GT(DictionaryLookup::composeSections(s, Section::All, true, offsets).size(), all.size());

  EXPECT_EQ(DictionaryLookup::composeSections(s, Section::Related, true, offsets), "");
  EXPECT_TRUE(offsets.empty());
}

TEST(DictionaryLookupSectionsTest, CompactDefinitionKeepsLeadingLines) {
  EXPECT_EQ(DictionaryLookup::compactDefinition("one\ntwo\n"), "one\ntwo");
  EXPECT_EQ(DictionaryLookup::compactDefinition("a\nb\nc\nd", 2), "a\nb\n...");
  EXPECT_EQ(DictionaryLookup::compactDefinition("short\n" + std::string(600, 'x'), 6, 480), "short\n...");
  // A late space is a word boundary; an early one is not worth the loss.
  const std::string late = std::string(100, 'y') + " tail end of a very long first line";
  EXPECT_EQ(DictionaryLookup::compactDefinition(late, 6, 120), std::string(100, 'y') + " tail end of a very...");
  const std::string early = "word " + std::string(200, 'y');
  EXPECT_EQ(DictionaryLookup::compactDefinition(early, 6, 120), "word " + std::string(115, 'y') + "...");
  std::string cjk;
  for (int i = 0; i < 50; ++i) cjk += "義";
  const std::string cjkCut = DictionaryLookup::compactDefinition(cjk, 6, 40);
  EXPECT_EQ(cjkCut, cjk.substr(0, 39) + "...");
}

TEST(DictionaryLookupSectionsTest, PagesNeverEndOnAHeaderOrStartBlank) {
  using K = LineKind;
  // H T T B H T T T B H T
  const std::vector<K> kinds = {K::Header, K::Text, K::Text,  K::Blank, K::Header, K::Text,
                                K::Text,   K::Text, K::Blank, K::Header, K::Text};
  // Four lines a page: page 1 would end "B" after two texts; page 2 starts at
  // the header, not the blank; the last header moves with its text.
  EXPECT_EQ(DictionaryLookup::paginateLines(kinds, 4), (std::vector<uint32_t>{0, 4, 9}));
  EXPECT_EQ(DictionaryLookup::paginateLines(kinds, 5), (std::vector<uint32_t>{0, 4, 9}));
  EXPECT_EQ(DictionaryLookup::paginateLines(kinds, 100), std::vector<uint32_t>{0});
  EXPECT_EQ(DictionaryLookup::paginateLines({}, 4), std::vector<uint32_t>{0});
  // A page that is all header still advances.
  EXPECT_EQ(DictionaryLookup::paginateLines({K::Header, K::Header, K::Text}, 1), (std::vector<uint32_t>{0, 1, 2}));
  EXPECT_EQ(DictionaryLookup::paginateLines({K::Text, K::Text, K::Text}, 0), (std::vector<uint32_t>{0, 1, 2}));
}

}  // namespace
