// Press-and-hold dictionary lookup: the hit test on laid-out page words, the
// lookup key it produces against a real StarDict fixture, and the rule that
// decides whether a hold looks up, says no dictionary is set, or leaves the
// contact to the page-turn zones.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "DictZip.h"
#include "Dictionary.h"
#include "DictionaryRegistry.h"
#include "StringUtils.h"
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
// other character.
int advance(const char* text, EpdFontFamily::Style) {
  int width = 0;
  for (const char* p = text; *p;) {
    uint32_t cp;
    const size_t n = CjkDictionary::characterBytes(p, cp);
    width += cp < 0x80 ? 10 : 24;
    p += n;
  }
  return width;
}

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

// --- Lookup against a real StarDict fixture --------------------------------

void be32(std::ostream& out, const uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) out.put(static_cast<char>(value >> shift));
}

class HoldLookup : public ::testing::Test {
 protected:
  void SetUp() override {
    dir = std::filesystem::temp_directory_path() /
          ("hold-lookup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(dir));
    const std::string stem = (dir / "fixture").string();
    std::vector<std::pair<std::string, std::string>> entries = {{"hello", "a greeting"},
                                                                {"well-known", "widely known"},
                                                                {"\xE7\xB3\xBB", "to tie"},
                                                                {"\xE7\xB3\xBB\xE7\xBB\x9F", "system"},
                                                                {"\xE7\xBB\x9F", "to unite"}};
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
    idx.close();
    data.close();
    std::ofstream(stem + ".ifo") << "StarDict's dict ifo file\nversion=2.4.2\nsametypesequence=m\n";
    ASSERT_TRUE(dict.open(stem.c_str()));
    if (dict.needsIndex()) ASSERT_TRUE(dict.buildIndex());
  }
  void TearDown() override { std::filesystem::remove_all(dir); }

  // What the direct lookup does with words[hit]; returns "headword=definition".
  std::string lookUp(const FakePage& page, const int x, const int y) {
    const int hit = page.hold(x, y);
    if (hit < 0) return "";
    char run[CjkDictionary::RUN_BYTES];
    DictionaryPageWords::cjkRun(page.words, hit, run);
    std::string definition;
    std::string headword;
    uint8_t mask = 0;
    const bool found = run[0] ? dict.lookupCjk(run, definition, headword, mask)
                              : dict.lookup(page.words[hit].text, definition, headword);
    return found ? headword + "=" + definition : "not found";
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
  EXPECT_EQ(lookUp(page, 30, 5), "\xE7\xBB\x9F=to unite");                        // 统 → 统
  EXPECT_EQ(lookUp(page, 52, 5), "not found");                                    // 很 is not in the fixture
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

}  // namespace
