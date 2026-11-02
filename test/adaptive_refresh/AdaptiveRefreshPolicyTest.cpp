#include <gtest/gtest.h>

#include <cstdint>

#include "AdaptiveRefreshPolicy.h"

namespace {

using Policy = AdaptiveRefreshPolicy;
using Refresh = Policy::Refresh;
using Reason = Policy::Reason;

constexpr uint32_t kPixels = 1000;  // 1000 pixels keeps every threshold (1500) an exact integer

// Exact last-displayed baseline available.
Policy::Inputs exact(uint32_t changed, uint32_t ink = 0) {
  Policy::Inputs in;
  in.frame.pixelCount = kPixels;
  in.frame.frameValid = true;
  in.frame.historyValid = true;
  in.frame.changedPixels = changed;
  in.frame.inkPixels = ink;
  return in;
}

// No baseline: only the current frame's ink is known.
Policy::Inputs scalar(uint32_t ink) {
  Policy::Inputs in;
  in.frame.pixelCount = kPixels;
  in.frame.frameValid = true;
  in.frame.historyValid = false;
  in.frame.inkPixels = ink;
  return in;
}

// decide -> (hardware does what the policy asked) -> commit.
Policy::Decision step(Policy& p, const Policy::Inputs& in) {
  const auto d = p.decide(in);
  p.commit(d.refresh);
  return d;
}

// Fast frames served before the policy first asks for a clear.
unsigned fastFramesBeforeClear(Policy& p, const Policy::Inputs& in, unsigned limit = 500) {
  for (unsigned n = 0; n < limit; ++n) {
    if (step(p, in).refresh == Refresh::Clear) return n;
  }
  return limit;
}

}  // namespace

TEST(AdaptiveRefreshPolicy, FirstFrameClearsUnlessQuickResumeExemptsIt) {
  Policy fresh;
  const auto first = step(fresh, exact(10));
  EXPECT_EQ(first.refresh, Refresh::Clear);
  EXPECT_EQ(first.reason, Reason::Initial);
  EXPECT_EQ(step(fresh, exact(10)).refresh, Refresh::Fast);

  Policy resumed(false);
  EXPECT_EQ(resumed.decide(exact(10)).refresh, Refresh::Fast);
}

TEST(AdaptiveRefreshPolicy, ClearIntervalFollowsChangedPixelsNotPageCount) {
  // Budget is 1.5 screens = 1500 pixels; the frame that would reach it clears.
  Policy dense(false);
  EXPECT_EQ(fastFramesBeforeClear(dense, exact(200)), 7u);  // 7*200=1400, the 8th reaches 1600

  Policy medium(false);
  EXPECT_EQ(fastFramesBeforeClear(medium, exact(100)), 14u);

  Policy sparse(false);
  EXPECT_EQ(fastFramesBeforeClear(sparse, exact(30)), 49u);  // far more turns, still debt-driven
}

TEST(AdaptiveRefreshPolicy, SafetyCapOnlyBoundsStreamsOfTinyChanges) {
  Policy p(false);
  for (unsigned i = 0; i < Policy::kSafetyCap; ++i) {
    ASSERT_EQ(step(p, exact(1)).refresh, Refresh::Fast) << "frame " << i;
  }
  const auto d = p.decide(exact(1));
  EXPECT_EQ(d.refresh, Refresh::Clear);
  EXPECT_EQ(d.reason, Reason::SafetyCap);
}

TEST(AdaptiveRefreshPolicy, DebtIsReportedAheadOfTheSafetyCapWhenBothApply) {
  Policy p(false);
  for (unsigned i = 0; i < Policy::kSafetyCap; ++i) p.commit(p.decide(exact(1)).refresh);
  auto image = exact(kPixels);
  image.hasImage = true;  // 2.0x -> 2000 on top of 60 already booked
  EXPECT_EQ(p.decide(image).reason, Reason::Debt);
}

TEST(AdaptiveRefreshPolicy, ClearingRefreshErasesDebtAndRestartsTheBudget) {
  Policy p(false);
  EXPECT_EQ(fastFramesBeforeClear(p, exact(200)), 7u);
  EXPECT_EQ(p.debtPixels(), 0u);
  EXPECT_EQ(p.fastSinceClear(), 0u);
  EXPECT_EQ(fastFramesBeforeClear(p, exact(200)), 7u);
}

TEST(AdaptiveRefreshPolicy, OnlyCommittedFramesAreCharged) {
  Policy p(false);
  for (int i = 0; i < 100; ++i) (void)p.decide(exact(200));  // never reached the panel
  EXPECT_EQ(p.debtPixels(), 0u);
  EXPECT_EQ(p.decide(exact(200)).refresh, Refresh::Fast);

  p.commit(Refresh::Fast);
  EXPECT_EQ(p.debtPixels(), 200u);
  p.commit(Refresh::Fast);  // a second commit has no pending frame to book
  EXPECT_EQ(p.debtPixels(), 200u);
  EXPECT_EQ(p.fastSinceClear(), 1u);

  Policy q(false);
  q.commit(Refresh::Fast);  // commit with no decide at all
  EXPECT_EQ(q.debtPixels(), 0u);
  EXPECT_EQ(q.fastSinceClear(), 0u);
}

TEST(AdaptiveRefreshPolicy, ImagesWeighHeavyButGrayTextAloneNeverForcesClears) {
  const uint32_t changed = 300;

  Policy plain(false);
  EXPECT_EQ(fastFramesBeforeClear(plain, exact(changed)), 4u);  // 300/frame, 5th reaches 1500

  Policy image(false);
  auto imageFrame = exact(changed);
  imageFrame.hasImage = true;  // 2.0x -> 600/frame
  EXPECT_EQ(fastFramesBeforeClear(image, imageFrame), 2u);

  Policy grayImage(false);
  auto grayImageFrame = exact(changed);
  grayImageFrame.hasImage = true;
  grayImageFrame.visibleGrayscale = true;  // 2.75x -> 825/frame
  EXPECT_EQ(fastFramesBeforeClear(grayImage, grayImageFrame), 1u);

  // AA text: 1.25x of a normal text page's change, so most of the fast run survives.
  Policy text(false);
  EXPECT_EQ(fastFramesBeforeClear(text, exact(130)), 11u);  // 130/frame
  Policy grayText(false);
  auto grayTextFrame = exact(130);
  grayTextFrame.visibleGrayscale = true;  // 162/frame
  EXPECT_EQ(fastFramesBeforeClear(grayText, grayTextFrame), 9u);
}

TEST(AdaptiveRefreshPolicy, MissingHistoryUsesInkEstimateInsteadOfClearingEveryPage) {
  // 60 ink pixels, previous frame unknown -> assumed alike -> 120 per frame.
  Policy p(false);
  EXPECT_EQ(fastFramesBeforeClear(p, scalar(60)), 12u);  // 12*120=1440, the 13th reaches 1560
}

TEST(AdaptiveRefreshPolicy, RecordedPreviousInkRefinesTheEstimate) {
  Policy p(false);
  EXPECT_EQ(step(p, scalar(100)).refresh, Refresh::Fast);  // previous unknown: 100+100
  EXPECT_EQ(p.debtPixels(), 200u);
  EXPECT_EQ(step(p, scalar(20)).refresh, Refresh::Fast);  // previous 100 recorded: 20+100
  EXPECT_EQ(p.debtPixels(), 320u);

  p.invalidateHistory();
  EXPECT_EQ(step(p, scalar(20)).refresh, Refresh::Fast);  // unknown again: 20+20
  EXPECT_EQ(p.debtPixels(), 360u);
}

TEST(AdaptiveRefreshPolicy, EstimateJudgesDarkBackgroundFramesBySparseSide) {
  Policy p(false);
  // 940 ink of 1000 is a dark-background page with 60 foreground pixels; it must
  // cost like a sparse white page, not like a nearly full-screen change.
  (void)step(p, scalar(940));
  EXPECT_EQ(p.debtPixels(), 120u);
  (void)step(p, scalar(940));
  EXPECT_EQ(p.debtPixels(), 240u);
}

TEST(AdaptiveRefreshPolicy, EstimateChargesAFullRewriteWhenBackgroundPolarityFlips) {
  Policy p(false);
  (void)step(p, scalar(60));  // white page
  EXPECT_EQ(p.debtPixels(), 120u);
  // Dark page after a white one: nearly every pixel changes, P - (60 + 60).
  EXPECT_EQ(p.estimateChangedPixels(scalar(940).frame), 880u);
  (void)step(p, scalar(940));
  EXPECT_EQ(p.debtPixels(), 1000u);
  // And back again, which exhausts the budget instead of looking like a sparse turn.
  EXPECT_EQ(p.decide(scalar(60)).refresh, Refresh::Clear);
}

TEST(AdaptiveRefreshPolicy, ExactHistoryWinsOverTheInkEstimate) {
  Policy p(false);
  (void)step(p, exact(10, 900));  // nearly identical frames, though both ink-heavy
  EXPECT_EQ(p.debtPixels(), 10u);
}

TEST(AdaptiveRefreshPolicy, RequestedClearIsMandatoryAndStickyUntilAClearCommits) {
  Policy p(false);
  p.requestClear();
  EXPECT_TRUE(p.clearPending());
  auto d = p.decide(exact(1));
  EXPECT_EQ(d.refresh, Refresh::Clear);
  EXPECT_EQ(d.reason, Reason::Requested);

  p.commit(Refresh::Fast);  // the clear did not actually run
  EXPECT_TRUE(p.clearPending());
  EXPECT_EQ(p.decide(exact(1)).reason, Reason::Requested);

  p.commit(Refresh::Clear);
  EXPECT_FALSE(p.clearPending());
  EXPECT_EQ(p.debtPixels(), 0u);
  EXPECT_EQ(p.decide(exact(1)).refresh, Refresh::Fast);
}

TEST(AdaptiveRefreshPolicy, ExternalClearPromotionAppliesToThatFrameOnly) {
  Policy p(false);
  auto promoted = exact(1);
  promoted.externalClearPending = true;
  const auto d = p.decide(promoted);
  EXPECT_EQ(d.refresh, Refresh::Clear);
  EXPECT_EQ(d.reason, Reason::External);
  EXPECT_EQ(p.decide(exact(1)).refresh, Refresh::Fast);
}

TEST(AdaptiveRefreshPolicy, HardwareClearThePolicyDidNotAskForStillResetsDebt) {
  Policy p(false);
  for (int i = 0; i < 5; ++i) (void)step(p, exact(200));
  EXPECT_EQ(p.debtPixels(), 1000u);
  ASSERT_EQ(p.decide(exact(200)).refresh, Refresh::Fast);
  p.commit(Refresh::Clear);  // e.g. renderer promoted the refresh to HALF
  EXPECT_EQ(p.debtPixels(), 0u);
  EXPECT_EQ(p.fastSinceClear(), 0u);
}

TEST(AdaptiveRefreshPolicy, UnreadableFrameAndPolarityChangeForceAClear) {
  Policy p(false);
  const Policy::Inputs unreadable;  // frameValid=false, pixelCount=0
  auto d = p.decide(unreadable);
  EXPECT_EQ(d.refresh, Refresh::Clear);
  EXPECT_EQ(d.reason, Reason::InvalidFrame);

  auto zeroSized = exact(0);
  zeroSized.frame.pixelCount = 0;
  EXPECT_EQ(p.decide(zeroSized).reason, Reason::InvalidFrame);

  auto inverted = exact(1);
  inverted.frame.polarityChangePending = true;
  d = p.decide(inverted);
  EXPECT_EQ(d.refresh, Refresh::Clear);
  EXPECT_EQ(d.reason, Reason::PolarityChange);
}

TEST(AdaptiveRefreshPolicy, CommittingAnUnreadableFrameForgetsThePreviousInk) {
  Policy p(false);
  (void)step(p, scalar(100));
  const Policy::Inputs unreadable;
  p.commit(p.decide(unreadable).refresh);  // Clear
  (void)step(p, scalar(20));               // previous unknown again: 20+20, not 20+100
  EXPECT_EQ(p.debtPixels(), 40u);
}

TEST(AdaptiveRefreshPolicy, ResetRestoresInitialStateAndHonorsTheQuickResumeExemption) {
  Policy p(false);
  for (int i = 0; i < 3; ++i) (void)step(p, exact(200));
  p.requestClear();
  p.reset(true);
  EXPECT_EQ(p.debtPixels(), 0u);
  EXPECT_EQ(p.fastSinceClear(), 0u);
  EXPECT_EQ(p.decide(exact(1)).reason, Reason::Initial);

  p.reset(false);
  EXPECT_FALSE(p.clearPending());
  EXPECT_EQ(p.decide(exact(1)).refresh, Refresh::Fast);
}

TEST(AdaptiveRefreshPolicy, HugeCountsSaturateInsteadOfWrapping) {
  Policy p(false);
  Policy::Inputs huge;
  huge.frame.pixelCount = 0xFFFFFFFFu;
  huge.frame.frameValid = true;
  huge.frame.historyValid = true;
  huge.frame.changedPixels = 0xFFFFFFFFu;
  huge.hasImage = true;
  huge.visibleGrayscale = true;

  // One saturated charge (2^32-1) is below the 1.5*2^32 threshold. A 32-bit
  // threshold would wrap to ~2^31 and wrongly clear here.
  EXPECT_EQ(p.decide(huge).refresh, Refresh::Fast);
  p.commit(Refresh::Fast);
  EXPECT_EQ(p.debtPixels(), 0xFFFFFFFFu);
  EXPECT_EQ(p.decide(huge).reason, Reason::Debt);
  p.commit(Refresh::Clear);
  EXPECT_EQ(p.debtPixels(), 0u);

  // A reported change above the panel size is clamped, never trusted.
  Policy clamped(false);
  (void)clamped.decide(exact(kPixels * 50));
  clamped.commit(Refresh::Fast);
  EXPECT_EQ(clamped.debtPixels(), kPixels);
}

TEST(AdaptiveRefreshPolicy, FastRunCounterSaturatesSoTheSafetyCapCannotReopen) {
  Policy p(false);
  for (uint32_t i = 0; i < 70000; ++i) {
    (void)p.decide(exact(0));
    p.commit(Refresh::Fast);  // forced fast regardless of the answer
  }
  EXPECT_EQ(p.fastSinceClear(), 0xFFFFu);
  EXPECT_EQ(p.decide(exact(0)).reason, Reason::SafetyCap);
}
