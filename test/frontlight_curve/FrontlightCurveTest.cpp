#include <cmath>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "FrontlightCurve.h"

namespace {

frontlight::Curve defaults() { return {}; }

uint32_t oldDuty(uint8_t percent) { return frontlight::legacyPerceptualDuty(percent, frontlight::kLegacyDutyFull); }

}  // namespace

TEST(FrontlightPwm, ChosenPairFitsRcFastAndTheRejectedOnesDoNot) {
  EXPECT_TRUE(frontlight::pwmValidOnRcFast(frontlight::kPwmHz, frontlight::kPwmBits));
  EXPECT_EQ(frontlight::kPwmHz, 3900u);
  EXPECT_EQ(frontlight::kPwmBits, 12u);
  EXPECT_EQ(frontlight::kLedcTimer, 0u);
  EXPECT_NE(frontlight::kLedcChannelCool, frontlight::kLedcChannelWarm);
  EXPECT_EQ(frontlight::kLedcChannelCool, 0u);
  EXPECT_EQ(frontlight::kLedcChannelWarm, 1u);

  // 4 kHz / 12-bit and 8 kHz / 11-bit fail the −7% RC_FAST floor. The stock
  // 25 kHz / 10-bit profile cannot clock from RC_FAST at any point in the band.
  EXPECT_FALSE(frontlight::pwmValidOnRcFast(4000, 12));
  EXPECT_FALSE(frontlight::pwmValidOnRcFast(8000, 11));
  EXPECT_FALSE(frontlight::pwmValidOnRcFast(25000, 10));
  EXPECT_TRUE(frontlight::pwmValidOnRcFast(7900, 11));

  const uint32_t slow = frontlight::kRcFastHzNominal - (frontlight::kRcFastHzNominal * 7) / 100;
  const uint32_t div = frontlight::ledcDivisor(slow, frontlight::kPwmHz, frontlight::kPwmBits);
  EXPECT_GE(div, frontlight::kLedcDivMin);
  EXPECT_LE(div, 280u);
}

TEST(FrontlightBrightness, StepsAreDistinctAndTheFloorIsOneLsb) {
  const auto curve = defaults();
  EXPECT_EQ(frontlight::brightnessDuty(0, frontlight::kDutyFull, curve), 0u);
  uint32_t previous = 0;
  for (uint8_t percent = 1; percent <= 100; ++percent) {
    const uint32_t duty = frontlight::brightnessDuty(percent, frontlight::kDutyFull, curve);
    EXPECT_GT(duty, previous) << "percent " << static_cast<int>(percent);
    previous = duty;
  }
  EXPECT_EQ(frontlight::brightnessDuty(0, frontlight::kDutyFull, curve), 0u);
  EXPECT_EQ(frontlight::brightnessDuty(1, frontlight::kDutyFull, curve), 1u);
  EXPECT_EQ(frontlight::brightnessDuty(100, frontlight::kDutyFull, curve), frontlight::kDutyFull);
  // Old 1% was 1 LSB of 1023, which is 4 LSB of 4095. That level is 2% now.
  EXPECT_EQ(frontlight::brightnessDuty(2, frontlight::kDutyFull, curve), 4u);
}

TEST(FrontlightBrightness, MidAndHighStayOnTheOldGamma) {
  const auto curve = defaults();
  double worst = 0;
  for (uint8_t percent = 30; percent <= 100; ++percent) {
    const double neu = static_cast<double>(frontlight::brightnessDuty(percent, frontlight::kDutyFull, curve)) /
                       frontlight::kDutyFull;
    const double old = static_cast<double>(oldDuty(percent)) / frontlight::kLegacyDutyFull;
    worst = std::max(worst, std::fabs(neu - old));
    EXPECT_EQ(frontlight::migrateBrightness(percent), percent) << "percent " << static_cast<int>(percent);
  }
  EXPECT_LT(worst, 0.001);
  EXPECT_EQ(frontlight::migrateBrightness(0), 0u);
  EXPECT_EQ(frontlight::migrateBrightness(1), 2u);
  EXPECT_EQ(frontlight::migrateBrightness(100), 100u);
}

TEST(FrontlightWarmth, TenPercentIsAThirdAndFullWarmthKillsCool) {
  const auto curve = defaults();
  EXPECT_EQ(frontlight::warmSharePermille(0, curve), 0u);
  EXPECT_EQ(frontlight::warmSharePermille(10, curve), 333u);
  EXPECT_EQ(frontlight::warmSharePermille(25, curve), 447u);
  EXPECT_EQ(frontlight::warmSharePermille(50, curve), 604u);
  EXPECT_EQ(frontlight::warmSharePermille(75, curve), 714u);
  EXPECT_EQ(frontlight::warmSharePermille(90, curve), 780u);
  EXPECT_EQ(frontlight::warmSharePermille(100, curve), 1000u);
  // The last 10% of the slider still has cool light to remove.
  EXPECT_GE(1000 - frontlight::warmSharePermille(90, curve), 200);

  for (uint8_t brightness = 0; brightness <= 100; ++brightness) {
    for (const uint8_t warmth : {uint8_t{0}, uint8_t{10}, uint8_t{25}, uint8_t{50}, uint8_t{75}, uint8_t{100}}) {
      const auto mix = frontlight::plan(brightness, warmth, frontlight::kDutyFull, true, true, curve);
      EXPECT_FALSE(mix.reconfigureTimer);
      EXPECT_EQ(mix.hpoint, 0u);
      EXPECT_EQ(mix.timerNum, 0u);
      EXPECT_EQ(mix.timerHz, frontlight::kPwmHz);
      EXPECT_EQ(mix.timerBits, frontlight::kPwmBits);
      EXPECT_NE(mix.coolChannel, mix.warmChannel);
      if (curve.warmEfficacy == frontlight::kWarmEfficacyDefault) {
        EXPECT_EQ(mix.coolLogical + mix.warmLogical, mix.totalDuty)
            << "brightness " << static_cast<int>(brightness) << " warmth " << static_cast<int>(warmth);
      }
      if (warmth == 100) {
        EXPECT_EQ(mix.coolLogical, 0u);
        EXPECT_EQ(mix.coolPad, 0u);
        EXPECT_EQ(mix.warmLogical, mix.totalDuty);
      }
      if (warmth == 0) {
        EXPECT_EQ(mix.warmLogical, 0u);
        EXPECT_EQ(mix.coolLogical, mix.totalDuty);
      }
    }
  }
}

TEST(FrontlightWarmth, LowBrightnessStillMixesWhenTheDutyCanSplit) {
  const auto curve = defaults();
  // 1 LSB cannot split. 10% warmth stays cool-only, and 100% is warm-only.
  const auto floor = frontlight::plan(1, 10, frontlight::kDutyFull, true, true, curve);
  EXPECT_EQ(floor.totalDuty, 1u);
  EXPECT_EQ(floor.coolLogical, 1u);
  EXPECT_EQ(floor.warmLogical, 0u);
  const auto floorWarm = frontlight::plan(1, 100, frontlight::kDutyFull, true, true, curve);
  EXPECT_EQ(floorWarm.coolPad, 0u);
  EXPECT_EQ(floorWarm.warmPad, 1u);

  // The migrated old 1% (duty 4) can put a warm LSB on at 10% warmth.
  const auto oldFloor = frontlight::plan(2, 10, frontlight::kDutyFull, true, true, curve);
  EXPECT_EQ(oldFloor.totalDuty, 4u);
  EXPECT_EQ(oldFloor.coolLogical, 3u);
  EXPECT_EQ(oldFloor.warmLogical, 1u);
  EXPECT_EQ(oldFloor.coolPad, 3u);

  for (uint8_t brightness = 1; brightness <= 100; ++brightness) {
    const auto mix = frontlight::plan(brightness, 10, frontlight::kDutyFull, true, true, curve);
    if (mix.totalDuty >= 4) {
      EXPECT_GT(mix.warmLogical, 0u) << "brightness " << static_cast<int>(brightness);
      EXPECT_GT(mix.coolLogical, 0u) << "brightness " << static_cast<int>(brightness);
    }
    const auto fullWarm = frontlight::plan(brightness, 100, frontlight::kDutyFull, true, true, curve);
    EXPECT_EQ(fullWarm.coolPad, 0u);
    EXPECT_EQ(fullWarm.hpoint, 0u);
  }
}

TEST(FrontlightWarmth, ActiveHighOffIsPadZeroAndActiveLowOffIsTheFullDuty) {
  const auto curve = defaults();
  const auto high = frontlight::plan(40, 100, frontlight::kDutyFull, true, true, curve);
  EXPECT_EQ(high.coolLogical, 0u);
  EXPECT_EQ(high.coolPad, 0u);
  EXPECT_EQ(high.hpoint, 0u);
  const auto low = frontlight::plan(40, 100, frontlight::kDutyFull, false, true, curve);
  EXPECT_EQ(low.coolLogical, 0u);
  EXPECT_EQ(low.coolPad, frontlight::kDutyFull);
}

TEST(FrontlightJson, InvalidTextFallsBackAndOutOfRangeClamps) {
  const auto missing = frontlight::parseCurveJson(nullptr, 0);
  EXPECT_FALSE(missing.fromFile);
  EXPECT_FLOAT_EQ(missing.brightnessGamma, frontlight::kBrightnessGamma);
  EXPECT_EQ(missing.warm[1].sharePermille, 333u);

  const auto garbage = frontlight::parseCurveJson("not json", 8);
  EXPECT_FALSE(garbage.fromFile);
  EXPECT_EQ(garbage.joinPercent, frontlight::kBrightnessJoinPercent);

  const std::string empty = "{}";
  const auto bare = frontlight::parseCurveJson(empty.c_str(), empty.size());
  EXPECT_TRUE(bare.fromFile);
  EXPECT_FALSE(bare.brightnessGammaOverridden);
  EXPECT_EQ(bare.warm[1].percent, 10u);

  const std::string wild =
      R"({"brightnessGamma":9,"lowGamma":0.1,"brightnessJoinPercent":90,"warmEfficacy":12,"unknown":4})";
  const auto clamped = frontlight::parseCurveJson(wild.c_str(), wild.size());
  EXPECT_TRUE(clamped.fromFile);
  EXPECT_FLOAT_EQ(clamped.brightnessGamma, 2.5f);
  EXPECT_TRUE(clamped.brightnessGammaOverridden);
  EXPECT_FLOAT_EQ(clamped.lowGamma, 1.2f);
  EXPECT_EQ(clamped.joinPercent, 50u);
  EXPECT_FLOAT_EQ(clamped.warmEfficacy, frontlight::kWarmEfficacyMax);
  EXPECT_EQ(clamped.warm[1].sharePermille, 333u);

  const std::string low = R"({"brightnessGamma":0.2,"warmEfficacy":0.1})";
  const auto lowClamped = frontlight::parseCurveJson(low.c_str(), low.size());
  EXPECT_FLOAT_EQ(lowClamped.brightnessGamma, 1.0f);
  EXPECT_FLOAT_EQ(lowClamped.warmEfficacy, frontlight::kWarmEfficacyMin);
}

TEST(FrontlightJson, BrokenWarmthOrderKeepsTheCompiledAnchors) {
  const std::string backwards = R"({"warmthLowPercent":80,"warmthLowShare":100,"warmthTailPercent":20})";
  const auto curve = frontlight::parseCurveJson(backwards.c_str(), backwards.size());
  EXPECT_TRUE(curve.fromFile);
  EXPECT_EQ(curve.warm[1].percent, frontlight::kDefaultWarmAnchors[1].percent);
  EXPECT_EQ(curve.warm[1].sharePermille, frontlight::kDefaultWarmAnchors[1].sharePermille);
  EXPECT_EQ(curve.warm[3].percent, frontlight::kDefaultWarmAnchors[3].percent);

  const std::string tuned = R"({"warmthLowPercent":10,"warmthLowShare":400})";
  const auto ok = frontlight::parseCurveJson(tuned.c_str(), tuned.size());
  EXPECT_EQ(ok.warm[1].percent, 10u);
  EXPECT_EQ(ok.warm[1].sharePermille, 400u);
  EXPECT_EQ(frontlight::warmSharePermille(10, ok), 400u);
  EXPECT_EQ(frontlight::warmSharePermille(100, ok), 1000u);
}

TEST(FrontlightJson, ExplicitDefaultsDoNotLeaveTheExactTable) {
  const std::string same = R"({"brightnessGamma":1.6554,"lowGamma":1.8,"brightnessJoinPercent":30})";
  const auto curve = frontlight::parseCurveJson(same.c_str(), same.size());
  EXPECT_FALSE(curve.brightnessGammaOverridden);
  EXPECT_FALSE(curve.lowGammaOverridden);
  EXPECT_EQ(frontlight::brightnessDuty(1, frontlight::kDutyFull, curve), 1u);
  EXPECT_EQ(frontlight::brightnessDuty(60, frontlight::kDutyFull, curve),
            frontlight::legacyPerceptualDuty(60, frontlight::kDutyFull));
}

TEST(FrontlightReport, TableCellsMatchTheDocumentedCurve) {
  const auto curve = defaults();
  const uint8_t brightnesses[] = {1, 2, 5, 10, 20, 50, 100};
  const uint32_t duties[] = {1, 4, 22, 77, 269, 1300, 4095};
  const uint8_t warmths[] = {0, 10, 25, 50, 75, 100};
  const uint32_t cool[7][6] = {
      {1, 1, 1, 0, 0, 0},       {4, 3, 2, 2, 1, 0},       {22, 15, 12, 9, 6, 0},      {77, 51, 43, 30, 22, 0},
      {269, 179, 149, 107, 77, 0}, {1300, 867, 719, 515, 372, 0}, {4095, 2731, 2265, 1622, 1171, 0},
  };
  const uint32_t warm[7][6] = {
      {0, 0, 0, 1, 1, 1},       {0, 1, 2, 2, 3, 4},       {0, 7, 10, 13, 16, 22},    {0, 26, 34, 47, 55, 77},
      {0, 90, 120, 162, 192, 269}, {0, 433, 581, 785, 928, 1300}, {0, 1364, 1830, 2473, 2924, 4095},
  };
  for (int row = 0; row < 7; ++row) {
    EXPECT_EQ(frontlight::brightnessDuty(brightnesses[row], frontlight::kDutyFull, curve), duties[row]);
    for (int col = 0; col < 6; ++col) {
      const auto mix = frontlight::plan(brightnesses[row], warmths[col], frontlight::kDutyFull, true, true, curve);
      EXPECT_EQ(mix.coolLogical, cool[row][col]) << row << "," << col;
      EXPECT_EQ(mix.warmLogical, warm[row][col]) << row << "," << col;
      EXPECT_EQ(mix.coolPad, cool[row][col]);
      EXPECT_EQ(mix.warmPad, warm[row][col]);
    }
  }
}
