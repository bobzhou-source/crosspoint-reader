#include <gtest/gtest.h>

#include "src/util/NightMode.h"

TEST(NightMode, ReaderAlwaysFollowsNightAndMenusCanOptOut) {
  for (bool night : {false, true}) {
    for (bool menus : {false, true}) {
      EXPECT_EQ(night_mode::invertActivity(night, menus, true), night);
      EXPECT_EQ(night_mode::invertActivity(night, menus, false), night && menus);
    }
  }
}

TEST(NightMode, BuiltinSleepCannotTurnWhiteInNightMode) {
  EXPECT_TRUE(night_mode::invertBuiltinSleep(true, true));
  EXPECT_TRUE(night_mode::invertBuiltinSleep(true, false));
  EXPECT_FALSE(night_mode::invertBuiltinSleep(false, true));
  EXPECT_TRUE(night_mode::invertBuiltinSleep(false, false));
}
