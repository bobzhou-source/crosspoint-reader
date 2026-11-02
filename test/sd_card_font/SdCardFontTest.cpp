#include <HalStorage.h>
#include <SdCardFont.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

namespace {
size_t failNextArraySize = 0;
}  // namespace

// Keep array allocation and deletion paired, including arrays allocated by the test framework.
void* operator new[](size_t size) {
  void* allocation = std::malloc(size == 0 ? 1 : size);
  if (!allocation) {
    std::fputs("Unexpected host OOM in SdCardFontTest\n", stderr);
    std::exit(EXIT_FAILURE);
  }
  return allocation;
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  if (failNextArraySize != 0 && size == failNextArraySize) {
    failNextArraySize = 0;
    return nullptr;
  }
  return std::malloc(size == 0 ? 1 : size);
}

void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation, size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, const std::nothrow_t&) noexcept { std::free(allocation); }

namespace {
constexpr uint32_t FIRST = 0xAC00;
constexpr uint32_t GLYPHS = 513;
constexpr uint16_t BITMAP_BYTES = 128;

void put16(size_t at, uint16_t value) {
  sdFontTestFile[at] = value;
  sdFontTestFile[at + 1] = value >> 8;
}
void put32(size_t at, uint32_t value) {
  put16(at, value);
  put16(at + 2, value >> 16);
}

// Style `s` advances 32 + 8*s px (12.4 fixed point) for every glyph, so a table that
// serves one style's lookup from another's fetch is visible in the value.
constexpr uint16_t styleAdvance(uint8_t style) { return static_cast<uint16_t>((32 + 8 * style) << 4); }

void makeFont(uint32_t glyphs = GLYPHS, uint8_t styleCount = 1) {
  constexpr size_t HEADER_BYTES = 32;
  constexpr size_t TOC_BYTES = 32;
  constexpr size_t INTERVAL_BYTES = 24;  // two 12-byte intervals
  const size_t styleBytes = INTERVAL_BYTES + glyphs * sizeof(EpdGlyph) + glyphs * BITMAP_BYTES;
  const size_t firstData = HEADER_BYTES + styleCount * TOC_BYTES;
  sdFontTestFile.assign(firstData + styleCount * styleBytes, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  sdFontTestFile[12] = styleCount;
  for (uint8_t style = 0; style < styleCount; ++style) {
    const size_t toc = HEADER_BYTES + style * TOC_BYTES;
    const size_t intervals = firstData + style * styleBytes;
    const size_t glyphTable = intervals + INTERVAL_BYTES;
    const size_t bitmaps = glyphTable + glyphs * sizeof(EpdGlyph);
    sdFontTestFile[toc] = style;
    put32(toc + 4, 2);
    put32(toc + 8, glyphs);
    sdFontTestFile[toc + 12] = 32;
    put16(toc + 13, 32);
    put32(toc + 24, intervals);
    put32(intervals, FIRST);
    put32(intervals + 4, FIRST + glyphs - 2);
    put32(intervals + 12, 0xFFFD);
    put32(intervals + 16, 0xFFFD);
    put32(intervals + 20, glyphs - 1);
    for (uint32_t i = 0; i < glyphs; ++i) {
      EpdGlyph glyph{};
      glyph.width = 32;
      glyph.height = 32;
      glyph.advanceX = styleAdvance(style);
      glyph.top = 32;
      glyph.dataLength = BITMAP_BYTES;
      // Store bitmaps in reverse glyph order to exercise sorted reads on rebuild.
      glyph.dataOffset = (glyphs - 1 - i) * BITMAP_BYTES;
      std::memcpy(sdFontTestFile.data() + glyphTable + i * sizeof(glyph), &glyph, sizeof(glyph));
      std::memset(sdFontTestFile.data() + bitmaps + glyph.dataOffset, i % 251, BITMAP_BYTES);
    }
  }
}

std::string page(uint32_t first, uint32_t count) {
  std::string text;
  text.reserve(count * 3);
  for (uint32_t cp = first; cp < first + count; ++cp) {
    text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
    text.push_back(static_cast<char>(0x80 | (cp & 63)));
  }
  return text;
}

uint32_t residentCount(SdCardFont& font) {
  const auto* data = font.getEpdFont()->data;
  uint32_t count = 0;
  for (uint32_t i = 0; i < data->intervalCount; ++i) {
    count += data->intervals[i].last - data->intervals[i].first + 1;
  }
  return count;
}

void expectPageBitmaps(SdCardFont& font, uint32_t first, uint32_t count) {
  const auto* data = font.getEpdFont()->data;
  for (uint32_t cp = first; cp < first + count; ++cp) {
    const EpdGlyph* glyph = nullptr;
    for (uint32_t i = 0; i < data->intervalCount; ++i) {
      const auto& interval = data->intervals[i];
      if (cp >= interval.first && cp <= interval.last) glyph = data->glyph + interval.offset + cp - interval.first;
    }
    ASSERT_NE(nullptr, glyph) << cp;
    ASSERT_EQ(BITMAP_BYTES, glyph->dataLength);
    for (uint16_t i = 0; i < BITMAP_BYTES; ++i) {
      ASSERT_EQ((cp - FIRST) % 251, data->bitmap[glyph->dataOffset + i]);
    }
  }
}
}  // namespace

TEST(SdCardFontTest, CompletePagesReplaceEarlierGlyphs) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  for (uint32_t offset : {0U, 100U, 200U}) {
    const uint32_t first = FIRST + offset;
    const auto text = page(first, 100);
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    EXPECT_EQ(101U, residentCount(font));  // includes replacement glyph
    expectPageBitmaps(font, first, 100);
  }
}

TEST(SdCardFontTest, IncrementalUiStringsStillAccumulate) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 5).c_str(), 1));
  ASSERT_EQ(0, font.prewarm(page(FIRST + 5, 5).c_str(), 1));
  EXPECT_EQ(11U, residentCount(font));
  expectPageBitmaps(font, FIRST, 10);
}

TEST(SdCardFontTest, UnderusedBuffersKeepTheFreshlyPrewarmedPageUntilItIsDrawn) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 200).c_str(), 1, false, false, false));
  font.clearCache();
  for (uint32_t offset : {300U, 400U, 200U, 0U}) {
    const auto text = page(FIRST + offset, 100);
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    font.clearCache();  // idle prewarm scope closes
    font.clearCache();  // actual page render scope opens
    sdFontTestReads = 0;
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    EXPECT_EQ(0U, sdFontTestReads);
    expectPageBitmaps(font, FIRST + offset, 100);
  }
}

TEST(SdCardFontTest, BitmapGrowthPreservesReadOrderAndAllGlyphData) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  for (uint32_t count : {50U, 100U, 200U, 400U}) {
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(page(FIRST, count).c_str(), 1, false, false, false));
    EXPECT_EQ(count + 1, residentCount(font));
    expectPageBitmaps(font, FIRST, count);
  }
}

TEST(SdCardFontTest, FragmentedBitmapGrowthRebuildsMetadataAndKeepsThePrefetch) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 50).c_str(), 1, false, false, false));
  struct HeapReportGuard {
    ~HeapReportGuard() { ESP.largestBlock = 200 * 1024; }
  } guard;
  ESP.largestBlock = 8 * 1024;
  const auto text = page(FIRST, 200);
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  expectPageBitmaps(font, FIRST, 200);
  font.clearCache();
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, BitmapAllocationRetriesAfterEvictingRebuildableAdvances) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  const auto text = page(FIRST, 200);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  ASSERT_TRUE(font.hasAdvanceTable());
  ASSERT_EQ(0, font.prewarm(page(FIRST, 50).c_str(), 1, false, false, false));
  struct AllocationGuard {
    ~AllocationGuard() {
      ESP.largestBlock = 200 * 1024;
      failNextArraySize = 0;
    }
  } guard;
  ESP.largestBlock = 8 * 1024;
  failNextArraySize = 201 * BITMAP_BYTES;
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(0U, failNextArraySize);
  EXPECT_FALSE(font.hasAdvanceTable());
  expectPageBitmaps(font, FIRST, 200);

  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  for (uint32_t cp = FIRST; cp < FIRST + 200; ++cp) {
    EXPECT_EQ(32 << 4, font.getAdvance(cp, 0));
  }
}

namespace {
// buildAdvanceTablePacked collects unique codepoints into a (4096 + 2)-entry scratch
// buffer. A build with nothing to fetch must not even ask for it; the probe turns that
// allocation into an observable failure (the build returns -1) and into a flag.
constexpr size_t ADVANCE_SCRATCH_BYTES = (4096 + 2) * sizeof(uint32_t);

struct ScratchAllocationProbe {
  ScratchAllocationProbe() { failNextArraySize = ADVANCE_SCRATCH_BYTES; }
  ~ScratchAllocationProbe() { failNextArraySize = 0; }
  bool untouched() const { return failNextArraySize == ADVANCE_SCRATCH_BYTES; }
};

void expectAdvances(const SdCardFont& font, uint32_t first, uint32_t count, uint8_t style) {
  for (uint32_t cp = first; cp < first + count; ++cp) {
    ASSERT_EQ(styleAdvance(style), font.getAdvance(cp, style)) << "cp " << cp << " style " << int(style);
  }
}
}  // namespace

TEST(SdCardFontTest, WarmAdvanceBuildsReadAndAllocateNothingAcrossRenderScopes) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  const auto text = page(FIRST, 200);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  expectAdvances(font, FIRST, 200, 0);

  sdFontTestReads = 0;
  // 0x0F names styles the font lacks; they resolve to the one present style, which is warm.
  for (const uint8_t mask : {uint8_t{1}, uint8_t{0x0F}}) {
    for (int pass = 0; pass < 2; ++pass) {
      font.clearCache();  // a closing render scope must not drop the advances
      ScratchAllocationProbe probe;
      EXPECT_EQ(0, font.buildAdvanceTable(text.c_str(), mask)) << "mask " << int(mask);
      EXPECT_TRUE(probe.untouched()) << "mask " << int(mask);
    }
  }
  EXPECT_EQ(0U, sdFontTestReads);
  expectAdvances(font, FIRST, 200, 0);
}

TEST(SdCardFontTest, PackedAdvanceBuildWarmsSpaceAndHyphenOnceThenStopsTouchingTheCard) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  // Two NUL-terminated words in one arena chunk, as the paragraph word arena packs them.
  const std::string words = page(FIRST, 20) + std::string(1, '\0') + page(FIRST + 20, 20) + std::string(1, '\0');
  const char* segments[] = {words.c_str()};
  const size_t lengths[] = {words.size()};

  ASSERT_EQ(0, font.buildAdvanceTablePacked(segments, lengths, 1, true, true, 1));
  expectAdvances(font, FIRST, 40, 0);
  // ' ' and '-' fall back to the replacement glyph; the build must have cached them.
  EXPECT_EQ(styleAdvance(0), font.getAdvance(' ', 0));
  EXPECT_EQ(styleAdvance(0), font.getAdvance('-', 0));

  sdFontTestReads = 0;
  ScratchAllocationProbe probe;
  EXPECT_EQ(0, font.buildAdvanceTablePacked(segments, lengths, 1, true, true, 1));
  EXPECT_TRUE(probe.untouched());
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, AdvanceBuildFetchesOnlyTheCodepointsItHasNotCached) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 100).c_str(), 1));

  // 50 resident + 50 new: only the new 50 may cost a card read.
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST + 50, 100).c_str(), 1));
  EXPECT_EQ(50U, sdFontTestReads);
  expectAdvances(font, FIRST, 150, 0);

  // A single unseen codepoint behind a warm page still falls to the full path.
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST + 149, 2).c_str(), 1));
  EXPECT_EQ(1U, sdFontTestReads);
  expectAdvances(font, FIRST, 151, 0);
}

TEST(SdCardFontTest, AdvanceTablesNeverCrossStyles) {
  makeFont(GLYPHS, 2);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  const auto text = page(FIRST, 200);

  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  expectAdvances(font, FIRST, 200, 0);
  for (uint32_t cp = FIRST; cp < FIRST + 200; ++cp) {
    ASSERT_EQ(0, font.getAdvance(cp, 1)) << "bold advances must not exist before bold was requested";
  }

  // Regular is warm, bold is cold: the regular table must not satisfy the bold request.
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 0x03));
  EXPECT_EQ(200U, sdFontTestReads);
  expectAdvances(font, FIRST, 200, 0);
  expectAdvances(font, FIRST, 200, 1);
  EXPECT_NE(styleAdvance(0), styleAdvance(1));

  // Both warm now.
  sdFontTestReads = 0;
  ScratchAllocationProbe probe;
  EXPECT_EQ(0, font.buildAdvanceTable(text.c_str(), 0x03));
  EXPECT_TRUE(probe.untouched());
  EXPECT_EQ(0U, sdFontTestReads);
  expectAdvances(font, FIRST, 200, 0);
  expectAdvances(font, FIRST, 200, 1);
}

TEST(SdCardFontTest, ClearCacheKeepsAdvancesButEveryExplicitReleaseDropsThemAndRefetchesIdentically) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  const auto text = page(FIRST, 100);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));

  font.clearCache();
  EXPECT_TRUE(font.hasAdvanceTable());
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  EXPECT_EQ(0U, sdFontTestReads);

  for (int release = 0; release < 2; ++release) {
    if (release == 0) {
      font.releaseResidentCaches();
    } else {
      font.clearPersistentCache();
    }
    EXPECT_FALSE(font.hasAdvanceTable()) << "release " << release;
    EXPECT_EQ(0, font.getAdvance(FIRST, 0)) << "release " << release;
    sdFontTestReads = 0;
    ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
    EXPECT_EQ(100U, sdFontTestReads) << "release " << release;
    expectAdvances(font, FIRST, 100, 0);
  }
}

TEST(SdCardFontTest, AdvanceCacheStopsAtItsLimitAndFullTablesCostNothing) {
  constexpr uint32_t LIMIT = 768;  // SdCardFont::ADVANCE_CACHE_LIMIT on hosts (no PSRAM)
  makeFont(900);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));

  // 800 unique codepoints against a 768-entry table: the table keeps the lowest 768.
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 800).c_str(), 1));
  expectAdvances(font, FIRST, LIMIT, 0);
  for (uint32_t cp = FIRST + LIMIT; cp < FIRST + 800; ++cp) {
    ASSERT_EQ(0, font.getAdvance(cp, 0)) << "past the cap, cp " << cp;
  }

  // Full table: more text is a no-op - no growth, no card reads, no scratch allocation.
  sdFontTestReads = 0;
  ScratchAllocationProbe probe;
  EXPECT_EQ(0, font.buildAdvanceTable(page(FIRST + 800, 50).c_str(), 1));
  EXPECT_TRUE(probe.untouched());
  EXPECT_EQ(0U, sdFontTestReads);
  for (uint32_t cp = FIRST + 800; cp < FIRST + 850; ++cp) {
    ASSERT_EQ(0, font.getAdvance(cp, 0)) << "table grew past its limit, cp " << cp;
  }
  expectAdvances(font, FIRST, LIMIT, 0);
}
