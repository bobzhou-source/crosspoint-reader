#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "src/activities/reader/PagePrerenderKey.h"

// The idle prerender may only stand in for a fresh render when nothing the
// render depends on has changed. These tests pin the invalidation key, the
// single-entry slot, which page gets prerendered, the grayscale plan rule and
// the spare-PSRAM admission policy.

namespace {

using namespace page_prerender;

Key baseKey() {
  Key k;
  k.bookId = 0xB00C;
  k.spineIndex = 4;
  k.pageIndex = 7;
  k.sectionPageCount = 31;
  k.spec.fontId = 12345;
  k.spec.lineCompression = 1.0f;
  k.spec.extraParagraphSpacing = true;
  k.spec.characterSpacing = 0;
  k.spec.wordSpacingPercent = 100;
  k.spec.paragraphAlignment = 1;
  k.spec.viewportWidth = 460;
  k.spec.viewportHeight = 760;
  k.spec.hyphenationEnabled = true;
  k.spec.embeddedStyle = true;
  k.spec.imageRendering = 0;
  k.spec.focusReadingEnabled = false;
  k.marginTop = 9;
  k.marginRight = 10;
  k.marginBottom = 35;
  k.marginLeft = 10;
  k.orientation = 0;
  k.screenInverted = false;
  k.textAntiAliasing = true;
  k.renderFontId = 98765;
  k.plan = Plan::BwAndPlanes;
  return k;
}

}  // namespace

TEST(EpubPrerenderKeyTest, EqualKeysMatch) {
  EXPECT_TRUE(baseKey() == baseKey());
  EXPECT_FALSE(baseKey() != baseKey());
}

TEST(EpubPrerenderKeyTest, EveryInputChangeMisses) {
  // One mutation per input the render depends on; each must break the match.
  const std::vector<std::pair<const char*, std::function<void(Key&)>>> mutations = {
      {"book", [](Key& k) { k.bookId += 1; }},
      {"spine", [](Key& k) { k.spineIndex += 1; }},
      {"page", [](Key& k) { k.pageIndex += 1; }},
      {"section page count", [](Key& k) { k.sectionPageCount += 1; }},
      {"font id", [](Key& k) { k.spec.fontId += 1; }},
      {"line spacing", [](Key& k) { k.spec.lineCompression = 1.15f; }},
      {"paragraph spacing", [](Key& k) { k.spec.extraParagraphSpacing = !k.spec.extraParagraphSpacing; }},
      {"character spacing", [](Key& k) { k.spec.characterSpacing = 1; }},
      {"word spacing", [](Key& k) { k.spec.wordSpacingPercent = 110; }},
      {"alignment", [](Key& k) { k.spec.paragraphAlignment = 2; }},
      {"viewport width", [](Key& k) { k.spec.viewportWidth += 1; }},
      {"viewport height", [](Key& k) { k.spec.viewportHeight += 1; }},
      {"hyphenation", [](Key& k) { k.spec.hyphenationEnabled = !k.spec.hyphenationEnabled; }},
      {"embedded style", [](Key& k) { k.spec.embeddedStyle = !k.spec.embeddedStyle; }},
      {"image rendering", [](Key& k) { k.spec.imageRendering = 1; }},
      {"focus reading", [](Key& k) { k.spec.focusReadingEnabled = true; }},
      {"margin top", [](Key& k) { k.marginTop += 1; }},
      {"margin right", [](Key& k) { k.marginRight += 1; }},
      {"margin bottom", [](Key& k) { k.marginBottom += 1; }},
      {"margin left", [](Key& k) { k.marginLeft += 1; }},
      {"orientation", [](Key& k) { k.orientation = 2; }},
      {"inversion", [](Key& k) { k.screenInverted = !k.screenInverted; }},
      {"anti-aliasing", [](Key& k) { k.textAntiAliasing = !k.textAntiAliasing; }},
      {"render font id", [](Key& k) { k.renderFontId += 1; }},
      {"plan", [](Key& k) { k.plan = Plan::BwOnly; }},
  };
  for (const auto& [name, mutate] : mutations) {
    Key changed = baseKey();
    mutate(changed);
    EXPECT_FALSE(changed == baseKey()) << name << " change must not match";
    EXPECT_TRUE(changed != baseKey()) << name << " change must differ";
  }
}

TEST(EpubPrerenderKeyTest, BookIdentityHashSeparatesBooks) {
  const std::string cacheA = "/.crosspoint/epub_1234";
  const std::string cacheB = "/.crosspoint/epub_5678";
  EXPECT_EQ(hashBytes(cacheA.data(), cacheA.size()), hashBytes(cacheA.data(), cacheA.size()));
  EXPECT_NE(hashBytes(cacheA.data(), cacheA.size()), hashBytes(cacheB.data(), cacheB.size()));
  EXPECT_NE(hashBytes("", 0), hashBytes("a", 1));
}

TEST(EpubPrerenderKeyTest, FreshSlotMatchesNothing) {
  Slot slot;
  EXPECT_FALSE(slot.valid());
  EXPECT_FALSE(slot.matches(baseKey()));
  EXPECT_FALSE(slot.matches(Key{}));
}

TEST(EpubPrerenderKeyTest, CommittedSlotMatchesOnlyItsKey) {
  Slot slot;
  ASSERT_TRUE(slot.commit(baseKey()));
  EXPECT_TRUE(slot.valid());
  EXPECT_TRUE(slot.matches(baseKey()));

  Key other = baseKey();
  other.pageIndex += 1;
  EXPECT_FALSE(slot.matches(other));
}

TEST(EpubPrerenderKeyTest, InvalidateDropsTheEntryEvenForAnEqualKey) {
  Slot slot;
  ASSERT_TRUE(slot.commit(baseKey()));
  slot.invalidate();
  EXPECT_FALSE(slot.valid());
  EXPECT_FALSE(slot.matches(baseKey()));

  // A new build after the invalidation is presentable again.
  ASSERT_TRUE(slot.commit(baseKey()));
  EXPECT_TRUE(slot.matches(baseKey()));
}

TEST(EpubPrerenderKeyTest, RecommitReplacesTheOldEntry) {
  Slot slot;
  ASSERT_TRUE(slot.commit(baseKey()));
  Key next = baseKey();
  next.pageIndex += 1;
  ASSERT_TRUE(slot.commit(next));
  EXPECT_TRUE(slot.matches(next));
  EXPECT_FALSE(slot.matches(baseKey()));  // single entry: the previous page is a miss
}

TEST(EpubPrerenderKeyTest, UnprerenderablePlanIsNeverCommitted) {
  Slot slot;
  ASSERT_TRUE(slot.commit(baseKey()));

  Key none = baseKey();
  none.plan = Plan::None;
  EXPECT_FALSE(slot.commit(none));
  EXPECT_FALSE(slot.valid());  // a refused commit also ends the previous entry
  EXPECT_FALSE(slot.matches(none));

  Key noSection = baseKey();
  noSection.spineIndex = -1;
  EXPECT_FALSE(slot.commit(noSection));
  Key noPage = baseKey();
  noPage.pageIndex = -1;
  EXPECT_FALSE(slot.commit(noPage));
  EXPECT_FALSE(slot.valid());
}

TEST(EpubPrerenderKeyTest, NextSectionKeyNeverMatchesASameSectionKey) {
  // Chapter boundary: page 0 of spine + 1 is not page + 1 of the same spine.
  const NextPage atEnd = nextPageAfter(4, 30, 31, 20);
  ASSERT_EQ(atEnd.kind, NextPage::Kind::NextSection);

  Key crossSection = baseKey();
  crossSection.spineIndex = atEnd.spineIndex;
  crossSection.pageIndex = atEnd.pageIndex;
  crossSection.sectionPageCount = 12;

  const NextPage mid = nextPageAfter(4, 7, 31, 20);
  ASSERT_EQ(mid.kind, NextPage::Kind::SameSection);
  Key sameSection = baseKey();
  sameSection.spineIndex = mid.spineIndex;
  sameSection.pageIndex = mid.pageIndex;

  Slot slot;
  ASSERT_TRUE(slot.commit(crossSection));
  EXPECT_TRUE(slot.matches(crossSection));
  EXPECT_FALSE(slot.matches(sameSection));
}

TEST(EpubPrerenderKeyTest, NextPageStaysInSectionUntilItsLastPage) {
  const NextPage mid = nextPageAfter(2, 0, 5, 10);
  EXPECT_EQ(mid.kind, NextPage::Kind::SameSection);
  EXPECT_EQ(mid.spineIndex, 2);
  EXPECT_EQ(mid.pageIndex, 1);

  const NextPage beforeLast = nextPageAfter(2, 3, 5, 10);
  EXPECT_EQ(beforeLast.kind, NextPage::Kind::SameSection);
  EXPECT_EQ(beforeLast.pageIndex, 4);
}

TEST(EpubPrerenderKeyTest, LastPageOfASectionPointsAtThePageZeroOfTheNextSection) {
  const NextPage next = nextPageAfter(2, 4, 5, 10);
  EXPECT_EQ(next.kind, NextPage::Kind::NextSection);
  EXPECT_EQ(next.spineIndex, 3);
  EXPECT_EQ(next.pageIndex, 0);
}

TEST(EpubPrerenderKeyTest, EndOfBookHasNoNextPage) {
  EXPECT_EQ(nextPageAfter(9, 4, 5, 10).kind, NextPage::Kind::None);
  // Single-section book on its final page.
  EXPECT_EQ(nextPageAfter(0, 0, 1, 1).kind, NextPage::Kind::None);
}

TEST(EpubPrerenderKeyTest, UnknownPositionHasNoNextPage) {
  EXPECT_EQ(nextPageAfter(-1, 0, 5, 10).kind, NextPage::Kind::None);
  EXPECT_EQ(nextPageAfter(2, -1, 5, 10).kind, NextPage::Kind::None);
  EXPECT_EQ(nextPageAfter(2, 0, 0, 10).kind, NextPage::Kind::None);  // section not loaded
  EXPECT_EQ(nextPageAfter(2, 5, 5, 10).kind, NextPage::Kind::None);  // page beyond the section
}

TEST(EpubPrerenderKeyTest, PlanFollowsTheGrayscaleRoute) {
  PlanInputs in;

  // No grayscale needed (night mode, or anti-aliasing off): B/W body only.
  EXPECT_EQ(planFor(in), Plan::BwOnly);

  // Anti-aliased text on the buffered tiled route: both planes are prerendered.
  in.needsAnyGrayscale = true;
  in.needsTextGrayscale = true;
  in.tiledGrayscale = true;
  in.overlapRefresh = true;
  EXPECT_EQ(planFor(in), Plan::BwAndPlanes);

  // Tiled route without the overlapped refresh does not use whole-plane buffers.
  in.overlapRefresh = false;
  EXPECT_EQ(planFor(in), Plan::None);

  // Non-tiled grayscale route.
  in.overlapRefresh = true;
  in.tiledGrayscale = false;
  EXPECT_EQ(planFor(in), Plan::None);
}

TEST(EpubPrerenderKeyTest, ImagePagesAreNeverPrerendered) {
  PlanInputs in;
  in.pageHasImages = true;
  EXPECT_EQ(planFor(in), Plan::None);

  in.needsAnyGrayscale = true;
  in.needsTextGrayscale = true;
  in.tiledGrayscale = true;
  in.overlapRefresh = true;
  EXPECT_EQ(planFor(in), Plan::None);

  PlanInputs absolute;
  absolute.absoluteImageGrayscale = true;
  EXPECT_EQ(planFor(absolute), Plan::None);
}

TEST(EpubPrerenderKeyTest, PsramAdmissionKeepsTheReserveFree) {
  constexpr size_t kBuffer = 48000;
  constexpr size_t kReserve = 1024 * 1024;

  // Exactly the buffers plus the reserve is enough; one byte less is not.
  EXPECT_TRUE(psramAdmits(kBuffer * 3 + kReserve, kBuffer * 3 + kReserve, kBuffer, 3));
  EXPECT_FALSE(psramAdmits(kBuffer * 3 + kReserve - 1, kBuffer * 3 + kReserve, kBuffer, 3));

  // Plenty free but fragmented below one buffer.
  EXPECT_FALSE(psramAdmits(8 * 1024 * 1024, kBuffer - 1, kBuffer, 1));
  EXPECT_TRUE(psramAdmits(8 * 1024 * 1024, kBuffer, kBuffer, 1));

  // The observed ~5.6 MB minimum free leaves ample room for three buffers.
  EXPECT_TRUE(psramAdmits(5600u * 1024u, 4 * 1024 * 1024, kBuffer, 3));

  // No PSRAM (zero heap), or a degenerate request, never admits.
  EXPECT_FALSE(psramAdmits(0, 0, kBuffer, 1));
  EXPECT_FALSE(psramAdmits(8 * 1024 * 1024, 8 * 1024 * 1024, 0, 1));
  EXPECT_FALSE(psramAdmits(8 * 1024 * 1024, 8 * 1024 * 1024, kBuffer, 0));
}

TEST(EpubPrerenderKeyTest, EvaluatesAtCompileTime) {
  static_assert(planFor(PlanInputs{}) == Plan::BwOnly);
  static_assert(nextPageAfter(1, 0, 2, 3).kind == NextPage::Kind::SameSection);
  static_assert(nextPageAfter(2, 1, 2, 3).kind == NextPage::Kind::None);
  static_assert(psramAdmits(2 * 1024 * 1024, 2 * 1024 * 1024, 48000, 1));
}
