#include <gtest/gtest.h>
#include <ButtonPressLatch.h>

namespace {
constexpr uint8_t LEFT = 1u << 2;
constexpr uint8_t RIGHT = 1u << 3;
}

TEST(InputLatch, ReleasedBeforePollingProducesExactlyOneClick) {
  ButtonPressLatch latch;
  latch.capture(RIGHT);
  EXPECT_EQ(latch.takeReleased(0, 0), RIGHT);
  EXPECT_EQ(latch.takeReleased(0, 0), 0);
}

TEST(InputLatch, ActiveObservationSurvivesUntilNormalDebounceCommits) {
  ButtonPressLatch latch;
  latch.capture(RIGHT);
  EXPECT_EQ(latch.takeReleased(RIGHT, 0), 0);
  EXPECT_TRUE(latch.hasPending());
  EXPECT_EQ(latch.takeReleased(RIGHT, RIGHT), 0);
  EXPECT_FALSE(latch.hasPending());
  EXPECT_EQ(latch.takeReleased(0, RIGHT), 0);
  EXPECT_EQ(latch.takeReleased(0, 0), 0);
}

TEST(InputLatch, ReleaseBetweenFirstAndSecondPollIsNotLost) {
  ButtonPressLatch latch;
  latch.capture(RIGHT);
  EXPECT_EQ(latch.takeReleased(RIGHT, 0), 0);
  EXPECT_EQ(latch.takeReleased(0, 0), RIGHT);
}

TEST(InputLatch, RepeatedYieldSamplesAndReleaseInterruptDoNotDuplicateNormalPress) {
  ButtonPressLatch latch;
  for (int i = 0; i < 20; ++i) latch.capture(RIGHT);
  EXPECT_EQ(latch.takeReleased(RIGHT, RIGHT), 0);
  latch.capture(RIGHT);
  EXPECT_EQ(latch.takeReleased(0, RIGHT), 0);
  EXPECT_EQ(latch.takeReleased(0, 0), 0);
}

TEST(InputLatch, IndependentButtonsPreservePhysicalMapping) {
  ButtonPressLatch latch;
  latch.capture(LEFT | RIGHT);
  EXPECT_EQ(latch.takeReleased(RIGHT, 0), LEFT);
  EXPECT_EQ(latch.takeReleased(0, 0), RIGHT);
}

TEST(InputLatch, ConsecutiveClicksEachArriveOnce) {
  ButtonPressLatch latch;
  for (int i = 0; i < 5; ++i) {
    latch.capture(RIGHT);
    EXPECT_EQ(latch.takeReleased(0, 0), RIGHT);
    EXPECT_EQ(latch.takeReleased(0, 0), 0);
  }
}

TEST(InputLatch, IdleNeverGeneratesAnEvent) {
  ButtonPressLatch latch;
  latch.capture(0);
  EXPECT_FALSE(latch.hasPending());
  EXPECT_EQ(latch.takeReleased(0, 0), 0);
}

TEST(InputPulse, RejectsChatterAndDebounceBoundary) {
  ButtonPulseFilter pulse;
  EXPECT_FALSE(pulse.edge(false, 0));
  EXPECT_FALSE(pulse.edge(true, 100));
  EXPECT_FALSE(pulse.edge(false, 5100));
  EXPECT_FALSE(pulse.edge(true, 6000));
  EXPECT_TRUE(pulse.edge(false, 12000));
  EXPECT_FALSE(pulse.edge(false, 13000));
}

TEST(InputPulse, LongUnpolledWorkDoesNotLoseShortPulse) {
  ButtonPulseFilter pulse;
  ButtonPressLatch latch;
  EXPECT_FALSE(pulse.edge(true, 100000));
  if (pulse.edge(false, 220000)) latch.capture(RIGHT);
  EXPECT_EQ(latch.takeReleased(0, 0), RIGHT);
  EXPECT_EQ(latch.takeReleased(0, 0), 0);
}

TEST(InputPulse, RepeatedActiveEdgesDoNotRestartTimer) {
  ButtonPulseFilter pulse;
  EXPECT_FALSE(pulse.edge(true, 100));
  EXPECT_FALSE(pulse.edge(true, 5100));
  EXPECT_TRUE(pulse.edge(false, 6100));
}

TEST(InputPulse, MicrosecondClockWrapIsSafe) {
  ButtonPulseFilter pulse;
  EXPECT_FALSE(pulse.edge(true, UINT32_MAX - 1000));
  EXPECT_TRUE(pulse.edge(false, 6000));
}
