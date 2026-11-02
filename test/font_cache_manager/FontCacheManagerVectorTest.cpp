#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <MemoryManager.h>
#include <SdCardFont.h>
#include <TtfEpdFont.h>
#include <Utf8.h>
#include <esp_heap_caps.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <new>
#include <string>
#include <utility>
#include <vector>

// FontCacheManager over the REAL TtfEpdFont with CROSSPOINT_VECTOR_FONTS live
// (the other FontCacheManager target compiles the vector paths out). Only the
// FreeType engine (FtFont), the two heap queries and the SDK memory manager are
// host stand-ins; every cache decision under test is production code:
//   g_ft.rasterLoads                 - FreeType glyph loads: what a cache MISS costs (one)
//   stream.calls                     - streamed-SD reads the loads cause
//   g_ft.kernQueries                 - kerning pairs that reached FreeType
//   glyph / bitmap bytes             - exact content served to the renderer
// Output compatibility against real FreeType/WenKai is established elsewhere
// (real-backend probe + emulator replay), not by this file.

namespace {

using freeink::font::g_ft;

bool countHeapAllocations = false;
size_t heapAllocationCount = 0;

constexpr int kFontId = 101;
constexpr int kSecondFontId = 202;
constexpr int kSdFontId = 7;
constexpr unsigned long kFileSize = 1ul << 20;
constexpr size_t kCacheBytes = 64 * 1024;
constexpr uint16_t kMaxGlyphs = 256;
constexpr uint16_t kPointSize = 14;
// Latin Extended-A/B: mapped by the stand-in cmap, never CJK-breakable, so kerning
// is not short-circuited by EpdFont::getKerning.
constexpr uint32_t kFirst = 0x100;

struct StreamCounter {
  size_t calls = 0;
  size_t bytes = 0;
};

unsigned long countingRead(void* ctx, unsigned long, unsigned char* buffer, const unsigned long count) {
  auto* counter = static_cast<StreamCounter*>(ctx);
  counter->calls++;
  counter->bytes += count;
  std::memset(buffer, 0, count);
  return count;
}

std::string text(const uint32_t first, const uint32_t count) {
  std::string out;
  for (uint32_t cp = first; cp < first + count; ++cp) utf8AppendCodepoint(cp, out);
  return out;
}

struct TextRun {
  const std::string& text;
  int fontId;
  EpdFontFamily::Style style;
};

struct Snapshot {
  bool present = false;
  EpdGlyph glyph{};
  std::vector<uint8_t> bitmap;
};

void expectSame(const Snapshot& a, const Snapshot& b, const uint32_t cp) {
  ASSERT_EQ(a.present, b.present) << cp;
  EXPECT_EQ(a.glyph.advanceX, b.glyph.advanceX) << cp;
  EXPECT_EQ(a.glyph.width, b.glyph.width) << cp;
  EXPECT_EQ(a.glyph.height, b.glyph.height) << cp;
  EXPECT_EQ(a.glyph.left, b.glyph.left) << cp;
  EXPECT_EQ(a.glyph.top, b.glyph.top) << cp;
  EXPECT_EQ(a.glyph.dataLength, b.glyph.dataLength) << cp;
  EXPECT_EQ(a.bitmap, b.bitmap) << cp;
}

uint32_t size26_6For(const uint16_t pointSize) { return (static_cast<uint32_t>(pointSize) * 150u * 64u + 36u) / 72u; }

// The load's linear 26.6 advance rounded to the 12.4 the renderer stores.
uint16_t advance12_4(const uint32_t cp, const uint16_t pointSize) {
  return static_cast<uint16_t>((freeink::font::FtFont::advance26_6For(cp, size26_6For(pointSize)) + 2) >> 2);
}

}  // namespace

void* operator new(const size_t size) {
  if (countHeapAllocations) heapAllocationCount++;
  if (void* allocation = std::malloc(size)) return allocation;
  throw std::bad_alloc();
}

void operator delete(void* allocation) noexcept { std::free(allocation); }

void operator delete(void* allocation, size_t) noexcept { std::free(allocation); }

class VectorFontCacheManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_ft = freeink::font::HostFtState{};
    g_hostHeap = HostHeapState{};
    freeink::MemoryManager::instance().onEvict = nullptr;
    freeink::MemoryManager::instance().ensureFreeCalls = 0;
    countHeapAllocations = false;

    ttf_.addStreamSource(TtfEpdFont::Regular, countingRead, &stream_, kFileSize);
    ASSERT_TRUE(loadFont(ttf_));
    fontMap_.emplace(kFontId, ttf_.family());
    ttfFonts_[kFontId] = &ttf_;
    manager_.setFontDecompressor(&decompressor_);
  }

  void TearDown() override { freeink::MemoryManager::instance().onEvict = nullptr; }

  static bool loadFont(TtfEpdFont& font, const uint16_t pointSize = kPointSize, const size_t cacheBytes = kCacheBytes,
                       const uint16_t maxGlyphs = kMaxGlyphs) {
    return font.load(pointSize, /*twoBit=*/true, cacheBytes, maxGlyphs);
  }

  void addSecondFont() {
    second_.addStreamSource(TtfEpdFont::Regular, countingRead, &secondStream_, kFileSize);
    ASSERT_TRUE(loadFont(second_));
    fontMap_.emplace(kSecondFontId, second_.family());
    ttfFonts_[kSecondFontId] = &second_;
  }

  // The reader's scan pass + prewarm, with no draw pass: exactly the glyphs the
  // scope's own warm-up caches.
  void warm(std::initializer_list<TextRun> runs) {
    auto scope = manager_.createPrewarmScope();
    for (const auto& run : runs) manager_.recordText(run.text.c_str(), run.fontId, run.style);
    scope.endScanAndPrewarm();
  }

  // One reader page: scan, prewarm, then the draw pass reading every glyph the way
  // renderCharImpl does (getGlyph on demand, bitmap through the vector handler).
  void renderPage(std::initializer_list<TextRun> runs) {
    auto scope = manager_.createPrewarmScope();
    for (const auto& run : runs) manager_.recordText(run.text.c_str(), run.fontId, run.style);
    scope.endScanAndPrewarm();
    for (const auto& run : runs) draw(run);
  }

  void draw(const TextRun& run) {
    const EpdFontFamily& family = fontMap_.at(run.fontId);
    const EpdFontData* data = family.getData(run.style);
    const auto* cursor = reinterpret_cast<const unsigned char*>(run.text.c_str());
    while (const uint32_t cp = utf8NextCodepoint(&cursor)) {
      const EpdGlyph* glyph = family.getGlyph(cp, run.style);
      if (glyph && glyph->dataLength) {
        const uint8_t* bytes = data->vectorBitmapHandler(data->glyphMissCtx, glyph);
        if (bytes) sink_ += bytes[0];
      }
    }
  }

  Snapshot snap(const uint32_t cp, const EpdFontFamily::Style style = EpdFontFamily::REGULAR, const int fontId = kFontId) {
    const EpdFontFamily& family = fontMap_.at(fontId);
    Snapshot s;
    const EpdGlyph* glyph = family.getGlyph(cp, style);
    if (!glyph) return s;
    s.present = true;
    s.glyph = *glyph;
    if (glyph->dataLength) {
      const EpdFontData* data = family.getData(style);
      const uint8_t* bytes = data->vectorBitmapHandler(data->glyphMissCtx, glyph);
      if (bytes) s.bitmap.assign(bytes, bytes + glyph->dataLength);
    }
    return s;
  }

  int8_t kern(const uint32_t left, const uint32_t right) {
    return fontMap_.at(kFontId).getKerning(left, right, EpdFontFamily::REGULAR);
  }

  StreamCounter stream_;
  StreamCounter secondStream_;
  TtfEpdFont ttf_;
  TtfEpdFont second_;
  FontDecompressor decompressor_;
  SdCardFont sdFont_;
  std::map<int, EpdFontFamily> fontMap_;
  std::map<int, SdCardFont*> sdFonts_;
  std::map<int, TtfEpdFont*> ttfFonts_;
  FontCacheManager manager_{fontMap_, sdFonts_, ttfFonts_};
  unsigned sink_ = 0;
};

// ---- retention across scopes -------------------------------------------------

TEST_F(VectorFontCacheManagerTest, WarmScopeCostsNoGlyphLoadsStreamReadsOrOutputChange) {
  const std::string page = text(kFirst, 24);
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(24u, g_ft.rasterLoads);  // ONE load per distinct glyph: advance and bitmap together
  EXPECT_EQ(24u, stream_.calls);

  std::vector<Snapshot> first;
  for (uint32_t cp = kFirst; cp < kFirst + 24; ++cp) first.push_back(snap(cp));

  for (int scope = 0; scope < 3; ++scope) renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(24u, g_ft.rasterLoads);
  EXPECT_EQ(24u, stream_.calls);

  for (uint32_t cp = kFirst; cp < kFirst + 24; ++cp) {
    const Snapshot again = snap(cp);
    EXPECT_GT(again.glyph.dataLength, 0) << cp;
    expectSame(first[cp - kFirst], again, cp);
  }
  EXPECT_EQ(24u, g_ft.rasterLoads);
}

TEST_F(VectorFontCacheManagerTest, ScopeBeginAndEndAloneDropNothing) {
  const std::string page = text(kFirst, 12);
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  const size_t loads = g_ft.rasterLoads;
  const size_t reads = stream_.calls;

  for (int i = 0; i < 3; ++i) {
    auto scope = manager_.createPrewarmScope();  // begin: reset
    scope.endScanAndPrewarm();                   // nothing recorded
  }                                              // end: reset again
  for (uint32_t cp = kFirst; cp < kFirst + 12; ++cp) EXPECT_TRUE(snap(cp).present) << cp;
  EXPECT_EQ(loads, g_ft.rasterLoads);
  EXPECT_EQ(reads, stream_.calls);
}

TEST_F(VectorFontCacheManagerTest, GrowingTextRasterizesOnlyAbsentGlyphs) {
  renderPage({{text(kFirst, 6), kFontId, EpdFontFamily::REGULAR}});  // 0x100..0x105
  EXPECT_EQ(6u, g_ft.rasterLoads);
  renderPage({{text(kFirst + 3, 6), kFontId, EpdFontFamily::REGULAR}});  // 0x103..0x108: three new
  EXPECT_EQ(9u, g_ft.rasterLoads);
  renderPage({{text(kFirst, 9), kFontId, EpdFontFamily::REGULAR}});  // everything already resident
  EXPECT_EQ(9u, g_ft.rasterLoads);
  EXPECT_EQ(9u, stream_.calls);  // each of those loads is the only read its glyph cost
}

TEST_F(VectorFontCacheManagerTest, EveryStyleGroupOfOneScopeIsRetainedNotReplacedByTheNextGroup) {
  const std::string regular = text(kFirst, 4);
  const std::string bold = text(kFirst + 0x10, 4);
  const std::string italic = text(kFirst + 0x20, 4);
  const std::string boldItalic = text(kFirst + 0x30, 4);

  // PrewarmScope warms its (font, style) groups highest style first and every
  // one is "the complete set for this render". For a TTF each group is only
  // added: none may flush what an earlier group, scope, or style warmed.
  warm({{regular, kFontId, EpdFontFamily::REGULAR},
        {bold, kFontId, EpdFontFamily::BOLD},
        {italic, kFontId, EpdFontFamily::ITALIC},
        {boldItalic, kFontId, EpdFontFamily::BOLD_ITALIC}});
  EXPECT_EQ(16u, g_ft.rasterLoads);

  // All 16 groups' glyphs are resident on the regular face: reading them costs nothing.
  for (const std::string* group : {&regular, &bold, &italic, &boldItalic}) {
    const auto* cursor = reinterpret_cast<const unsigned char*>(group->c_str());
    while (const uint32_t cp = utf8NextCodepoint(&cursor)) EXPECT_GT(snap(cp).glyph.dataLength, 0) << cp;
  }
  EXPECT_EQ(16u, g_ft.rasterLoads);

  // Lazy style faces still serve their OWN design (fingerprint = high nibble of the
  // first bitmap byte: bold pixel / italic pixel), never the regular face's copy.
  const uint32_t cp = kFirst + 0x10;
  ASSERT_FALSE(snap(cp, EpdFontFamily::REGULAR).bitmap.empty());
  EXPECT_EQ(0x0, snap(cp, EpdFontFamily::REGULAR).bitmap[0] >> 4);
  EXPECT_EQ(0xC, snap(cp, EpdFontFamily::BOLD).bitmap[0] >> 4);
  EXPECT_EQ(0x3, snap(cp, EpdFontFamily::ITALIC).bitmap[0] >> 4);
  EXPECT_EQ(0xF, snap(cp, EpdFontFamily::BOLD_ITALIC).bitmap[0] >> 4);

  // The first drawn page faults the lazy style faces' own glyphs (they are never
  // prewarmed); every page after that re-rasterizes nothing.
  const auto drawFullPage = [&] {
    renderPage({{regular, kFontId, EpdFontFamily::REGULAR},
                {bold, kFontId, EpdFontFamily::BOLD},
                {italic, kFontId, EpdFontFamily::ITALIC},
                {boldItalic, kFontId, EpdFontFamily::BOLD_ITALIC}});
  };
  drawFullPage();
  const size_t loads = g_ft.rasterLoads;
  const size_t reads = stream_.calls;
  drawFullPage();
  drawFullPage();
  EXPECT_EQ(loads, g_ft.rasterLoads);
  EXPECT_EQ(reads, stream_.calls);
}

TEST_F(VectorFontCacheManagerTest, TwoVectorFontsInOneScopeKeepTheirOwnWorkingSets) {
  addSecondFont();
  const std::string a = text(kFirst, 8);
  const std::string b = text(kFirst + 0x40, 8);

  renderPage({{a, kFontId, EpdFontFamily::REGULAR}, {b, kSecondFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(8u, stream_.calls);  // one streamed glyph read per distinct glyph
  EXPECT_EQ(8u, secondStream_.calls);

  renderPage({{a, kFontId, EpdFontFamily::REGULAR}, {b, kSecondFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(8u, stream_.calls);
  EXPECT_EQ(8u, secondStream_.calls);

  // The explicit clear reaches every registered vector font.
  manager_.clearCache();
  renderPage({{a, kFontId, EpdFontFamily::REGULAR}, {b, kSecondFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(16u, stream_.calls);
  EXPECT_EQ(16u, secondStream_.calls);
}

TEST_F(VectorFontCacheManagerTest, ZeroInkAndUnmappedCodepointsDoNotRefaultEveryScope) {
  std::string page = " a";
  utf8AppendCodepoint(freeink::font::FtFont::kUnmappedCodepoint, page);  // no cmap entry -> replacement glyph
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  const size_t loads = g_ft.rasterLoads;
  const size_t reads = stream_.calls;
  EXPECT_EQ(3u, g_ft.rasterLoads);  // space, 'a', and the replacement glyph the unmapped cp falls back to

  const Snapshot space = snap(freeink::font::FtFont::kSpace);
  EXPECT_TRUE(space.present);
  EXPECT_EQ(0, space.glyph.dataLength);  // zero ink, but advance known
  EXPECT_GT(space.glyph.advanceX, 0);

  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(loads, g_ft.rasterLoads);
  EXPECT_EQ(reads, stream_.calls);
}

// ---- kerning -----------------------------------------------------------------

TEST_F(VectorFontCacheManagerTest, KernPairsSurviveScopesAndStayCapped) {
  const uint32_t lefts[3] = {kFirst, kFirst + 1, kFirst + 2};
  const uint32_t rights[3] = {kFirst + 0x20, kFirst + 0x21, kFirst + 0x22};
  int8_t values[3];
  for (int i = 0; i < 3; ++i) values[i] = kern(lefts[i], rights[i]);
  EXPECT_EQ(3u, g_ft.kernQueries);

  renderPage({{text(kFirst, 8), kFontId, EpdFontFamily::REGULAR}});
  renderPage({{text(kFirst, 8), kFontId, EpdFontFamily::REGULAR}});
  for (int i = 0; i < 3; ++i) EXPECT_EQ(values[i], kern(lefts[i], rights[i])) << i;
  EXPECT_EQ(3u, g_ft.kernQueries);  // served from the cache across two scopes

  // 600 further distinct pairs overflow the 512-pair cap (which drops the whole
  // table and refills): bounded, and still correct.
  const auto left = [](const uint32_t i) { return kFirst + 0x40 + i / 24; };
  const auto right = [](const uint32_t i) { return kFirst + 0x80 + i % 24; };
  const int8_t firstValue = kern(left(0), right(0));
  for (uint32_t i = 1; i < 600; ++i) kern(left(i), right(i));
  EXPECT_EQ(3u + 600u, g_ft.kernQueries);

  renderPage({{text(kFirst, 8), kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(603u, g_ft.kernQueries);
  kern(left(599), right(599));  // newest pair: resident across the scope
  EXPECT_EQ(603u, g_ft.kernQueries);
  EXPECT_EQ(firstValue, kern(left(0), right(0)));  // evicted by the cap, re-queried with the same answer
  EXPECT_EQ(604u, g_ft.kernQueries);
}

// ---- explicit invalidation ---------------------------------------------------

TEST_F(VectorFontCacheManagerTest, ExplicitClearCacheDropsGlyphsAndKerningAndRefaultsIdentically) {
  const std::string page = text(kFirst, 10);
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  const int8_t k = kern(kFirst, kFirst + 1);
  std::vector<Snapshot> before;
  for (uint32_t cp = kFirst; cp < kFirst + 10; ++cp) before.push_back(snap(cp));
  EXPECT_EQ(10u, g_ft.rasterLoads);
  EXPECT_EQ(1u, g_ft.kernQueries);

  manager_.clearCache();  // explicit: not a scope boundary
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(20u, g_ft.rasterLoads);
  EXPECT_EQ(k, kern(kFirst, kFirst + 1));
  EXPECT_EQ(2u, g_ft.kernQueries);
  for (uint32_t cp = kFirst; cp < kFirst + 10; ++cp) expectSame(before[cp - kFirst], snap(cp), cp);
  EXPECT_EQ(20u, g_ft.rasterLoads);
}

TEST_F(VectorFontCacheManagerTest, ReleaseSdFontCachesFreesTheVectorWorkingSetAndLazyFaces) {
  const std::string regular = text(kFirst, 4);
  const std::string bold = text(kFirst + 0x10, 4);
  renderPage({{regular, kFontId, EpdFontFamily::REGULAR}, {bold, kFontId, EpdFontFamily::BOLD}});
  EXPECT_EQ(12u, g_ft.rasterLoads);  // 8 warmed on the regular face + 4 faulted by the bold face
  EXPECT_EQ(2u, g_ft.inits);         // regular at load, bold lazily on first bold draw
  const size_t pageReads = stream_.calls;
  const Snapshot regularBefore = snap(kFirst);
  const Snapshot boldBefore = snap(kFirst + 0x10, EpdFontFamily::BOLD);

  manager_.releaseSdFontCaches();
  EXPECT_EQ(1u, g_ft.deinits);  // the lazy bold face was shed; the regular face stays live

  renderPage({{regular, kFontId, EpdFontFamily::REGULAR}, {bold, kFontId, EpdFontFamily::BOLD}});
  EXPECT_EQ(24u, g_ft.rasterLoads);  // nothing survived the release
  EXPECT_EQ(3u, g_ft.inits);         // bold face rebuilt on demand
  EXPECT_EQ(2 * pageReads, stream_.calls);
  expectSame(regularBefore, snap(kFirst), kFirst);
  expectSame(boldBefore, snap(kFirst + 0x10, EpdFontFamily::BOLD), kFirst + 0x10);
}

TEST_F(VectorFontCacheManagerTest, ReloadAtANewSizeInvalidatesRetainedGlyphs) {
  const std::string page = text(kFirst, 6);
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(6u, g_ft.rasterLoads);
  EXPECT_EQ(advance12_4(kFirst, 14), snap(kFirst).glyph.advanceX);

  ASSERT_TRUE(loadFont(ttf_, /*pointSize=*/20));  // reader size change: load() is the invalidation
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(12u, g_ft.rasterLoads);
  EXPECT_EQ(advance12_4(kFirst, 20), snap(kFirst).glyph.advanceX);
  EXPECT_NE(advance12_4(kFirst, 14), advance12_4(kFirst, 20));

  // ... and the new size is then retained like any other.
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(12u, g_ft.rasterLoads);
}

// ---- bounds ------------------------------------------------------------------

TEST_F(VectorFontCacheManagerTest, GlyphCapStillBoundsTheRetainedSet) {
  ASSERT_TRUE(loadFont(ttf_, kPointSize, kCacheBytes, /*maxGlyphs=*/8));
  warm({{text(kFirst, 20), kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(20u, g_ft.rasterLoads);

  // The newest glyph is resident (the cap flushes BEFORE inserting); the first is not.
  EXPECT_GT(snap(kFirst + 19).glyph.dataLength, 0);
  EXPECT_EQ(20u, g_ft.rasterLoads);
  EXPECT_GT(snap(kFirst).glyph.dataLength, 0);
  EXPECT_EQ(21u, g_ft.rasterLoads);  // re-faulted: 20 glyphs cannot all fit in 8 slots
}

TEST_F(VectorFontCacheManagerTest, ByteCapStillBoundsTheRetainedSet) {
  ASSERT_TRUE(loadFont(ttf_, kPointSize, /*cacheBytes=*/64, kMaxGlyphs));
  warm({{text(kFirst, 10), kFontId, EpdFontFamily::REGULAR}});  // >= 120 bytes of 2-bit glyphs into 64
  EXPECT_EQ(10u, g_ft.rasterLoads);

  EXPECT_GT(snap(kFirst + 9).glyph.dataLength, 0);
  EXPECT_EQ(10u, g_ft.rasterLoads);
  EXPECT_GT(snap(kFirst).glyph.dataLength, 0);
  EXPECT_EQ(11u, g_ft.rasterLoads);
}

// ---- one load per miss, and recovery from a failed one -----------------------

TEST_F(VectorFontCacheManagerTest, EveryMissIsOneGlyphLoadAndKeepsTheFractionalAdvance) {
  const std::string page = text(kFirst, 6);
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(6u, g_ft.rasterLoads);  // advance and bitmap come from the same load
  EXPECT_EQ(6u, stream_.calls);
  for (uint32_t cp = kFirst; cp < kFirst + 6; ++cp) {
    const Snapshot s = snap(cp);
    // The load's linear 26.6 advance rounded to 12.4, not the integer-pixel advance of the bitmap.
    EXPECT_EQ(advance12_4(cp, kPointSize), s.glyph.advanceX) << cp;
    EXPECT_GT(s.glyph.dataLength, 0) << cp;
  }
  EXPECT_EQ(6u, g_ft.rasterLoads);
}

TEST_F(VectorFontCacheManagerTest, AGlyphWhoseRenderFailsKeepsItsAdvanceAndIsRetriedByTheNextPrewarmOnly) {
  const uint32_t bad = kFirst + 2;
  const std::string page = text(kFirst, 6);
  g_ft.failRaster.insert(bad);
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(6u, g_ft.rasterLoads);
  const Snapshot blank = snap(bad);
  ASSERT_TRUE(blank.present);
  EXPECT_EQ(0, blank.glyph.dataLength);                             // draws blank ...
  EXPECT_EQ(advance12_4(bad, kPointSize), blank.glyph.advanceX);  // ... but layout keeps its advance

  draw({page, kFontId, EpdFontFamily::REGULAR});  // nothing retries mid-scope
  EXPECT_EQ(6u, g_ft.rasterLoads);

  g_ft.failRaster.clear();
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});  // the next prewarm rebuilds the degraded face
  EXPECT_EQ(12u, g_ft.rasterLoads);
  const Snapshot healed = snap(bad);
  EXPECT_GT(healed.glyph.dataLength, 0);
  EXPECT_EQ(blank.glyph.advanceX, healed.glyph.advanceX);

  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});  // healthy again: retained like any other face
  EXPECT_EQ(12u, g_ft.rasterLoads);
}

TEST_F(VectorFontCacheManagerTest, AGlyphWhoseLoadFailsHasNoAdvanceAndIsRetriedByTheNextPrewarm) {
  const uint32_t bad = kFirst + 1;
  const std::string page = text(kFirst, 4);
  g_ft.failLoad.insert(bad);
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(4u, g_ft.rasterLoads);
  const Snapshot failed = snap(bad);
  ASSERT_TRUE(failed.present);
  EXPECT_EQ(0, failed.glyph.dataLength);
  EXPECT_EQ(0, failed.glyph.advanceX);  // the load never produced one

  g_ft.failLoad.clear();
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(8u, g_ft.rasterLoads);
  EXPECT_EQ(advance12_4(bad, kPointSize), snap(bad).glyph.advanceX);
  EXPECT_GT(snap(bad).glyph.dataLength, 0);
}

TEST_F(VectorFontCacheManagerTest, ArenaGrowFailureDegradesToAdvanceOnlyThenRecoversAtTheNextPrewarm) {
  const std::string page = text(kFirst, 6);
  // Funds the glyph tables but not the first 4 KB bitmap-arena step.
  g_hostHeap.spiramLargest = 3000;
  g_hostHeap.internalLargest = 3000;
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_GT(freeink::MemoryManager::instance().ensureFreeCalls, 0u);  // eviction was tried first
  EXPECT_EQ(6u, g_ft.rasterLoads);
  for (uint32_t cp = kFirst; cp < kFirst + 6; ++cp) {
    const Snapshot s = snap(cp);
    ASSERT_TRUE(s.present) << cp;
    EXPECT_EQ(0, s.glyph.dataLength) << cp;
    EXPECT_EQ(advance12_4(cp, kPointSize), s.glyph.advanceX) << cp;  // layout survives
  }

  g_hostHeap = HostHeapState{};
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(12u, g_ft.rasterLoads);
  for (uint32_t cp = kFirst; cp < kFirst + 6; ++cp) EXPECT_GT(snap(cp).glyph.dataLength, 0) << cp;

  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(12u, g_ft.rasterLoads);
}

TEST_F(VectorFontCacheManagerTest, ADegradedLazyStyleFaceIsRetriedByThePrewarmAndTheHealthyRegularFaceIsNot) {
  const std::string regular = text(kFirst, 4);
  const std::string bold = text(kFirst + 0x10, 4);
  const uint32_t bad = kFirst + 0x11;

  warm({{regular, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(4u, g_ft.rasterLoads);
  const Snapshot regularBefore = snap(kFirst);

  g_ft.failRaster.insert(bad);
  draw({bold, kFontId, EpdFontFamily::BOLD});  // lazily inits the bold face and faults its own glyphs
  EXPECT_EQ(8u, g_ft.rasterLoads);
  EXPECT_EQ(0, snap(bad, EpdFontFamily::BOLD).glyph.dataLength);

  g_ft.failRaster.clear();
  warm({{regular, kFontId, EpdFontFamily::REGULAR}});  // regular is healthy and resident: no loads
  EXPECT_EQ(8u, g_ft.rasterLoads);
  EXPECT_EQ(2u, g_ft.inits);  // the rebuild flushes the bold face, it does not tear it down

  draw({bold, kFontId, EpdFontFamily::BOLD});  // the bold face was rebuilt: its glyphs fault again, now healthy
  EXPECT_EQ(12u, g_ft.rasterLoads);
  EXPECT_GT(snap(bad, EpdFontFamily::BOLD).glyph.dataLength, 0);
  expectSame(regularBefore, snap(kFirst), kFirst);

  warm({{regular, kFontId, EpdFontFamily::REGULAR}});
  draw({bold, kFontId, EpdFontFamily::BOLD});
  EXPECT_EQ(12u, g_ft.rasterLoads);
}

TEST_F(VectorFontCacheManagerTest, AGlyphLargerThanTheWholeByteCapStaysAdvanceOnlyAndIsNotRetried) {
  ASSERT_TRUE(loadFont(ttf_, kPointSize, /*cacheBytes=*/8, kMaxGlyphs));  // every 2-bit stand-in glyph needs >= 12
  const std::string page = text(kFirst, 6);
  warm({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(6u, g_ft.rasterLoads);
  for (uint32_t cp = kFirst; cp < kFirst + 6; ++cp) {
    const Snapshot s = snap(cp);
    ASSERT_TRUE(s.present) << cp;
    EXPECT_EQ(0, s.glyph.dataLength) << cp;
    EXPECT_EQ(advance12_4(cp, kPointSize), s.glyph.advanceX) << cp;
  }

  // Not a failure a retry could fix: the next prewarm must not rebuild the face.
  warm({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(6u, g_ft.rasterLoads);
}

TEST_F(VectorFontCacheManagerTest, ACapFlushDiscardsTheDegradedMarkWithTheEntries) {
  ASSERT_TRUE(loadFont(ttf_, kPointSize, kCacheBytes, /*maxGlyphs=*/4));
  g_ft.failRaster.insert(kFirst);
  warm({{text(kFirst, 1), kFontId, EpdFontFamily::REGULAR}});  // one blank, degraded entry
  EXPECT_EQ(1u, g_ft.rasterLoads);
  g_ft.failRaster.clear();

  // Five more glyphs against four slots: the cap flushes mid-draw and takes the blank entry with it.
  draw({text(kFirst + 0x10, 5), kFontId, EpdFontFamily::REGULAR});
  EXPECT_EQ(6u, g_ft.rasterLoads);

  // The newest glyph is resident and the face is healthy: this prewarm rebuilds nothing.
  warm({{text(kFirst + 0x14, 1), kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(6u, g_ft.rasterLoads);
}

TEST_F(VectorFontCacheManagerTest, BuildReplacesTheRegularFaceWhileAddCoverageOnlyAdds) {
  const std::string a = text(kFirst, 4);
  const std::string b = text(kFirst + 2, 4);
  const std::string c = text(kFirst + 0x20, 2);
  ASSERT_TRUE(ttf_.addCoverage(a.c_str()));
  EXPECT_EQ(4u, g_ft.rasterLoads);
  ASSERT_TRUE(ttf_.addCoverage(b.c_str()));
  EXPECT_EQ(6u, g_ft.rasterLoads);  // only the two glyphs a did not have

  ASSERT_TRUE(ttf_.build(c.c_str()));  // an explicit seed replaces the face's contents
  EXPECT_EQ(8u, g_ft.rasterLoads);
  EXPECT_GT(snap(kFirst + 0x20).glyph.dataLength, 0);
  EXPECT_EQ(8u, g_ft.rasterLoads);
  EXPECT_GT(snap(kFirst).glyph.dataLength, 0);
  EXPECT_EQ(9u, g_ft.rasterLoads);  // a's glyph was replaced, so it faults again
  EXPECT_FALSE(ttf_.addCoverage(nullptr));
}

// ---- memory pressure ---------------------------------------------------------

TEST_F(VectorFontCacheManagerTest, PressureReleaseDuringAGlyphFaultLeavesTheFaultingFontIntact) {
  // 64 glyphs fill the first table reservation exactly, so glyph 65 needs a heap-checked grow.
  warm({{text(kFirst, 64), kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(64u, g_ft.rasterLoads);

  g_hostHeap.spiramLargest = 0;
  g_hostHeap.internalLargest = 0;
  int sinkRuns = 0;
  // GfxRenderer's eviction sinks, as ensureFree() runs them: drop the render glyph
  // cache and release every vector font's arenas.
  freeink::MemoryManager::instance().onEvict = [&] {
    ++sinkRuns;
    manager_.releaseSdFontCaches();
    ttf_.releaseResidentCaches();
  };
  EXPECT_EQ(nullptr, fontMap_.at(kFontId).getGlyph(kFirst + 64, EpdFontFamily::REGULAR));  // no heap: uncached miss
  EXPECT_GT(freeink::MemoryManager::instance().ensureFreeCalls, 0u);
  EXPECT_EQ(static_cast<int>(freeink::MemoryManager::instance().ensureFreeCalls), sinkRuns);

  // The sink ran mid-fault against the font that was faulting: it must have been a
  // no-op there, so every glyph resident before the failed grow still is.
  g_hostHeap = HostHeapState{};
  freeink::MemoryManager::instance().onEvict = nullptr;
  const size_t loads = g_ft.rasterLoads;
  for (uint32_t cp = kFirst; cp < kFirst + 64; ++cp) EXPECT_GT(snap(cp).glyph.dataLength, 0) << cp;
  EXPECT_EQ(loads, g_ft.rasterLoads);

  // The lock only spans the fault: the same release outside one frees everything.
  manager_.releaseSdFontCaches();
  EXPECT_GT(snap(kFirst).glyph.dataLength, 0);
  EXPECT_EQ(loads + 1, g_ft.rasterLoads);
}

// ---- built-in/SD transient state and scan state ------------------------------

TEST_F(VectorFontCacheManagerTest, ScopesStillResetBuiltInAndSdTransientCachesWhileTtfIsRetained) {
  sdFonts_[kSdFontId] = &sdFont_;
  const std::string page = text(kFirst, 8);
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  const size_t loads = g_ft.rasterLoads;

  sdFont_.cached = "stale";
  decompressor_.pagesResident = true;
  {
    auto scope = manager_.createPrewarmScope();
    EXPECT_TRUE(sdFont_.cached.empty());  // scope entry resets SD + built-in transient state
    EXPECT_FALSE(decompressor_.pagesResident);

    manager_.recordText("ab", kSdFontId, EpdFontFamily::REGULAR);
    manager_.recordText(page.c_str(), kFontId, EpdFontFamily::REGULAR);
    scope.endScanAndPrewarm();
    EXPECT_EQ("ab", sdFont_.cached);  // SD prewarm replaces; TTF dispatch left it alone
    decompressor_.pagesResident = true;
  }
  EXPECT_TRUE(sdFont_.cached.empty());  // scope exit resets them again
  EXPECT_FALSE(decompressor_.pagesResident);
  EXPECT_EQ(loads, g_ft.rasterLoads);  // ... while the vector font kept every glyph
}

TEST_F(VectorFontCacheManagerTest, ScanStateDoesNotLeakAcrossScopesOrMoves) {
  const std::string page = text(kFirst, 5);
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}});
  EXPECT_EQ(5u, g_ft.rasterLoads);

  manager_.clearCache();
  {
    auto scope = manager_.createPrewarmScope();  // empty scope after a clear: nothing to replay
  }
  EXPECT_EQ(5u, g_ft.rasterLoads);

  {
    auto original = manager_.createPrewarmScope();
    manager_.recordText(page.c_str(), kFontId, EpdFontFamily::REGULAR);
    auto moved = std::move(original);
    EXPECT_EQ(5u, g_ft.rasterLoads);  // nothing warmed until the live scope ends
  }                                   // the moved-from scope is inert; the live one warms once
  EXPECT_EQ(10u, g_ft.rasterLoads);

  manager_.clearCache();
  {
    auto scope = manager_.createPrewarmScope();  // the finished scan left no codepoints behind
  }
  EXPECT_EQ(10u, g_ft.rasterLoads);
}

// ---- allocation --------------------------------------------------------------

TEST_F(VectorFontCacheManagerTest, WarmScopeAllocatesNoHeapMemory) {
  const std::string page = text(kFirst, 24);
  const std::string bold = text(kFirst + 0x10, 8);
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}, {bold, kFontId, EpdFontFamily::BOLD}});  // cold: grows caches

  heapAllocationCount = 0;
  countHeapAllocations = true;
  renderPage({{page, kFontId, EpdFontFamily::REGULAR}, {bold, kFontId, EpdFontFamily::BOLD}});
  countHeapAllocations = false;
  EXPECT_EQ(0u, heapAllocationCount);
}
