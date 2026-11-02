#include <ButtonPressLatch.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {
constexpr uint8_t LEFT = 1u << 2;
constexpr uint8_t RIGHT = 1u << 3;

// Model of the X4 Pro active loop (src/main.cpp loop(): gpio.update(), the
// activity's work, then the pause; InputManager::update()'s button path). The
// debounce, edge capture, click latch and wait policy are the firmware classes;
// FreeRTOS time advances on its 1 ms tick, so a pause of n ticks ends at the
// n-th tick boundary and an edge interrupt ends an InputEdge pause at once.
enum class Wake { FixedDelay, InputEdge };  // delay(10) vs HalGPIO::waitForInput(10)

struct Trace {
  std::vector<uint32_t> pollUs, pressUs, releaseUs, repeatUs;
};

Trace runLoop(const Wake wake, const std::vector<uint32_t>& edgesUs, const uint32_t endUs,
              const uint32_t workUs = 300) {
  constexpr unsigned long kPeriodMs = 10;
  constexpr unsigned long kContinuousStartMs = 500;     // ButtonNavigator defaults
  constexpr unsigned long kContinuousIntervalMs = 500;  // (continuousStartMs/IntervalMs)
  Trace trace;
  ButtonDebounce debounce;
  ButtonPulseFilter pulse;
  ButtonPressLatch latch;
  uint8_t committed = 0;
  unsigned long pressStartMs = 0;
  unsigned long lastContinuousMs = 0;
  size_t nextEdge = 0;
  bool edgeGiven = false;
  // Edges alternate pressed/released starting from released.
  const auto rawAt = [&](const uint32_t us) {
    size_t toggles = 0;
    while (toggles < edgesUs.size() && edgesUs[toggles] <= us) ++toggles;
    return (toggles % 2) ? RIGHT : uint8_t{0};
  };
  const auto runIsrsThrough = [&](const uint32_t us) {
    for (; nextEdge < edgesUs.size() && edgesUs[nextEdge] <= us; ++nextEdge) {
      if (pulse.edge(nextEdge % 2 == 0, edgesUs[nextEdge])) latch.capture(RIGHT);
      edgeGiven = true;  // captureButtonEdge() gives the input edge semaphore
    }
  };

  uint32_t t = 0;
  while (t < endUs) {
    runIsrsThrough(t);
    trace.pollUs.push_back(t);
    const unsigned long nowMs = t / 1000;
    const uint8_t raw = rawAt(t);
    const uint8_t previouslyCommitted = committed;
    uint8_t pressed = 0;
    uint8_t released = 0;
    if (debounce.settled(raw, nowMs) && raw != committed) {
      pressed = static_cast<uint8_t>(raw & ~committed);
      released = static_cast<uint8_t>(committed & ~raw);
      if (pressed && committed == 0) pressStartMs = nowMs;
      committed = raw;
    }
    const uint8_t clicks = latch.takeReleased(raw, static_cast<uint8_t>(previouslyCommitted | committed));
    pressed = static_cast<uint8_t>(pressed | clicks);
    released = static_cast<uint8_t>(released | clicks);
    if (pressed) trace.pressUs.push_back(t);
    if (released) {
      trace.releaseUs.push_back(t);
      lastContinuousMs = 0;
    }
    // ButtonNavigator::onContinuous()/shouldNavigateContinuously().
    if ((committed & RIGHT) && nowMs - pressStartMs > kContinuousStartMs &&
        nowMs - lastContinuousMs > kContinuousIntervalMs) {
      trace.repeatUs.push_back(t);
      lastContinuousMs = nowMs;
    }

    t += workUs;
    runIsrsThrough(t);
    const uint32_t tickUs = t / 1000 * 1000;
    if (wake == Wake::FixedDelay) {
      t = tickUs + kPeriodMs * 1000;
      continue;
    }
    if (edgeGiven) {  // an edge during the work: xSemaphoreTake() returns at once
      edgeGiven = false;
      continue;
    }
    const bool pending = debounce.lastRaw() != committed;
    const unsigned long waitMs = inputPollWaitMs(pending, debounce.msUntilSettled(t / 1000), kPeriodMs);
    const uint32_t timeoutUs = tickUs + static_cast<uint32_t>(waitMs) * 1000;
    if (nextEdge < edgesUs.size() && edgesUs[nextEdge] < timeoutUs) {
      t = edgesUs[nextEdge];
      runIsrsThrough(t);
      edgeGiven = false;  // consumed by the take it ended
    } else {
      t = timeoutUs;
    }
  }
  return trace;
}

uint32_t pollsBetween(const Trace& trace, const uint32_t fromUs, const uint32_t toUs) {
  uint32_t polls = 0;
  for (const uint32_t poll : trace.pollUs) polls += poll >= fromUs && poll < toUs;
  return polls;
}
}  // namespace

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

TEST(InputDebounce, CommitsOnlyAfterMoreThanTheDelayAndSurvivesWrap) {
  ButtonDebounce debounce;
  EXPECT_FALSE(debounce.settled(RIGHT, 100));
  EXPECT_EQ(debounce.msUntilSettled(100), 6u);
  EXPECT_FALSE(debounce.settled(RIGHT, 105));
  EXPECT_EQ(debounce.msUntilSettled(105), 1u);
  EXPECT_TRUE(debounce.settled(RIGHT, 106));
  EXPECT_EQ(debounce.msUntilSettled(106), 0u);
  EXPECT_FALSE(debounce.settled(0, 107));  // a new change restarts the window
  EXPECT_FALSE(debounce.settled(0, 112));
  EXPECT_TRUE(debounce.settled(0, 113));

  ButtonDebounce wrapped;
  EXPECT_FALSE(wrapped.settled(RIGHT, ULONG_MAX - 2));
  EXPECT_FALSE(wrapped.settled(RIGHT, 2));
  EXPECT_TRUE(wrapped.settled(RIGHT, 3));
}

TEST(InputDebounce, SyncAdoptsAHoldStyleCommitWithoutRestartingTheWindow) {
  ButtonDebounce debounce;
  debounce.sync(LEFT);
  EXPECT_EQ(debounce.lastRaw(), LEFT);
  EXPECT_TRUE(debounce.settled(LEFT, 50));  // no raw change since construction
}

TEST(InputWait, QuietLoopSleepsTheWholePeriodAndPendingChangeOnlyUntilItCanCommit) {
  EXPECT_EQ(inputPollWaitMs(false, 0, 10), 10u);
  EXPECT_EQ(inputPollWaitMs(false, 4, 10), 10u);
  EXPECT_EQ(inputPollWaitMs(true, 6, 10), 6u);
  EXPECT_EQ(inputPollWaitMs(true, 0, 10), 1u);  // never a zero-tick busy spin
  EXPECT_EQ(inputPollWaitMs(true, 25, 10), 10u);
}

TEST(InputWake, BounceShorterThanTheDebounceNeverProducesAPress) {
  for (const Wake wake : {Wake::FixedDelay, Wake::InputEdge}) {
    for (const uint32_t widthUs : {100u, 1000u, 2500u, 4000u, 4999u}) {
      for (uint32_t phaseUs = 0; phaseUs < 10000; phaseUs += 125) {
        const uint32_t at = 50000 + phaseUs;
        const Trace trace = runLoop(wake, {at, at + widthUs}, 200000);
        EXPECT_TRUE(trace.pressUs.empty()) << "width " << widthUs << " phase " << phaseUs;
        EXPECT_TRUE(trace.releaseUs.empty()) << "width " << widthUs << " phase " << phaseUs;
      }
    }
    for (uint32_t phaseUs = 0; phaseUs < 10000; phaseUs += 125) {
      const uint32_t at = 50000 + phaseUs;  // chatter burst spanning 4.7 ms
      const Trace trace = runLoop(wake, {at, at + 300, at + 800, at + 1500, at + 2100, at + 4700}, 200000);
      EXPECT_TRUE(trace.pressUs.empty()) << "burst phase " << phaseUs;
    }
  }
}

TEST(InputWake, BouncyPressAndReleaseReportExactlyOneEach) {
  for (const Wake wake : {Wake::FixedDelay, Wake::InputEdge}) {
    for (uint32_t phaseUs = 0; phaseUs < 10000; phaseUs += 125) {
      const uint32_t p = 50000 + phaseUs;
      const uint32_t r = p + 120000;
      const Trace trace =
          runLoop(wake, {p, p + 400, p + 900, p + 1600, p + 2000, r, r + 500, r + 1100}, 300000);
      EXPECT_EQ(trace.pressUs.size(), 1u) << "phase " << phaseUs;
      EXPECT_EQ(trace.releaseUs.size(), 1u) << "phase " << phaseUs;
    }
  }
}

TEST(InputWake, EdgeWakeCommitsAPressWithinTheDebounceWindow) {
  uint32_t edgeWakeMaxUs = 0;
  uint32_t fixedDelayMinUs = UINT32_MAX;
  for (uint32_t phaseUs = 0; phaseUs < 10000; phaseUs += 50) {
    const uint32_t p = 50000 + phaseUs;
    const Trace edge = runLoop(Wake::InputEdge, {p, p + 100000}, 200000);
    const Trace fixed = runLoop(Wake::FixedDelay, {p, p + 100000}, 200000);
    ASSERT_EQ(edge.pressUs.size(), 1u);
    ASSERT_EQ(fixed.pressUs.size(), 1u);
    edgeWakeMaxUs = std::max(edgeWakeMaxUs, edge.pressUs[0] - p);
    fixedDelayMinUs = std::min(fixedDelayMinUs, fixed.pressUs[0] - p);
    EXPECT_GT(edge.pressUs[0] - p, ButtonDebounce::kDelayMs * 1000) << "phase " << phaseUs;
  }
  EXPECT_LE(edgeWakeMaxUs, (ButtonDebounce::kDelayMs + 2) * 1000);
  EXPECT_LT(edgeWakeMaxUs, fixedDelayMinUs);
}

TEST(InputWake, HeldButtonRepeatsAtTheSameCadenceAndPollRate) {
  for (uint32_t phaseUs = 0; phaseUs < 10000; phaseUs += 250) {
    const uint32_t p = 50000 + phaseUs;
    const uint32_t r = p + 3000000;
    const Trace edge = runLoop(Wake::InputEdge, {p, r}, r + 100000);
    const Trace fixed = runLoop(Wake::FixedDelay, {p, r}, r + 100000);
    ASSERT_EQ(edge.repeatUs.size(), fixed.repeatUs.size()) << "phase " << phaseUs;
    ASSERT_GE(edge.repeatUs.size(), 2u);
    EXPECT_GE(edge.repeatUs[0] - edge.pressUs[0], 500000u);
    for (size_t i = 1; i < edge.repeatUs.size(); ++i) {
      EXPECT_EQ(edge.repeatUs[i] - edge.repeatUs[i - 1], fixed.repeatUs[i] - fixed.repeatUs[i - 1]);
    }
    // While held (nothing pending, no edges) the loop wakes once per period.
    const uint32_t from = edge.pressUs[0] + 20000;
    EXPECT_EQ(pollsBetween(edge, from, from + 2000000), 200u);
    EXPECT_EQ(pollsBetween(fixed, from, from + 2000000), 200u);
  }
}

TEST(InputWake, NoInputWakesExactlyAsOftenAsTheFixedDelay) {
  const Trace edge = runLoop(Wake::InputEdge, {}, 2000000);
  const Trace fixed = runLoop(Wake::FixedDelay, {}, 2000000);
  EXPECT_EQ(edge.pollUs, fixed.pollUs);
  EXPECT_EQ(pollsBetween(edge, 1000000, 2000000), 100u);
}

TEST(InputWake, ChatterAddsOnlyABoundedNumberOfWakes) {
  const std::vector<uint32_t> burst = {50000, 50300, 50800, 51500, 52100, 54700};
  const Trace edge = runLoop(Wake::InputEdge, burst, 200000);
  const Trace quiet = runLoop(Wake::InputEdge, {}, 200000);
  // One wake per edge plus at most two debounce re-polls per edge.
  EXPECT_LE(edge.pollUs.size(), quiet.pollUs.size() + 3 * burst.size());
  // The burst ends before the next 10 ms poll and the cadence resumes.
  EXPECT_EQ(pollsBetween(edge, 100000, 200000), 10u);
}
