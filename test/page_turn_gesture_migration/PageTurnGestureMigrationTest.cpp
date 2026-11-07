// One-time page-turn gesture upgrade: a pre-schema file carrying the old
// untouched SWIPE_ONLY pair switches to TAP_AND_SWIPE, while a file that has
// already been through the schema (including an explicit SWIPE_ONLY choice)
// is left alone.
#include <gtest/gtest.h>

#include "src/PageTurnGestureMigration.h"

namespace {

constexpr uint8_t TAP_AND_SWIPE = 0;
constexpr uint8_t TAP_ONLY = 1;
constexpr uint8_t SWIPE_ONLY = 2;
constexpr uint8_t INVERTED_TAP = 3;

bool upgrade(uint8_t& next, uint8_t& previous, const bool schemaPresent) {
  return page_turn_gesture::upgradeLegacyDefault(next, previous, schemaPresent, SWIPE_ONLY, TAP_AND_SWIPE);
}

}  // namespace

TEST(PageTurnGestureMigration, UntouchedLegacyPairBecomesTapAndSwipe) {
  uint8_t next = SWIPE_ONLY;
  uint8_t previous = SWIPE_ONLY;
  EXPECT_TRUE(upgrade(next, previous, false));
  EXPECT_EQ(next, TAP_AND_SWIPE);
  EXPECT_EQ(previous, TAP_AND_SWIPE);
}

TEST(PageTurnGestureMigration, PostSchemaSwipeOnlyChoiceSurvives) {
  // The user explicitly chose SWIPE_ONLY after the schema bump: both
  // directions are SWIPE_ONLY again, and the marker must keep them there.
  uint8_t next = SWIPE_ONLY;
  uint8_t previous = SWIPE_ONLY;
  EXPECT_FALSE(upgrade(next, previous, true));
  EXPECT_EQ(next, SWIPE_ONLY);
  EXPECT_EQ(previous, SWIPE_ONLY);
}

TEST(PageTurnGestureMigration, CustomizedPreSchemaPairsArePreserved) {
  for (const auto [nextValue, previousValue] :
       {std::pair{TAP_ONLY, TAP_ONLY}, std::pair{SWIPE_ONLY, TAP_ONLY}, std::pair{TAP_AND_SWIPE, SWIPE_ONLY},
        std::pair{INVERTED_TAP, INVERTED_TAP}}) {
    uint8_t next = nextValue;
    uint8_t previous = previousValue;
    EXPECT_FALSE(upgrade(next, previous, false))
        << static_cast<int>(nextValue) << "/" << static_cast<int>(previousValue);
    EXPECT_EQ(next, nextValue);
    EXPECT_EQ(previous, previousValue);
  }
}
