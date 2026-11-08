#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

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

TEST(AdaptiveRefreshPolicyConfig, ThresholdFromConfigSetsTheClearCadence) {
  Policy::Config config;
  config.clearThresholdPermille = 3000;  // twice the compiled budget
  Policy relaxed(false, config);
  EXPECT_EQ(fastFramesBeforeClear(relaxed, exact(200)), 14u);  // 14*200=2800, the 15th reaches 3000

  config.clearThresholdPermille = 400;
  Policy strict(false, config);
  EXPECT_EQ(fastFramesBeforeClear(strict, exact(200)), 1u);
}

TEST(AdaptiveRefreshPolicyConfig, NightThresholdAppliesOnlyToInvertedOutput) {
  Policy::Config config;
  config.clearThresholdPermille = 1000;
  config.nightClearThresholdPermille = 2000;
  auto night = exact(200);
  night.invertedOutput = true;

  Policy day(false, config);
  EXPECT_EQ(fastFramesBeforeClear(day, exact(200)), 4u);
  Policy dark(false, config);
  EXPECT_EQ(fastFramesBeforeClear(dark, night), 9u);

  // Compiled defaults: both polarities share one budget.
  Policy compiledDay(false);
  Policy compiledNight(false);
  EXPECT_EQ(fastFramesBeforeClear(compiledDay, exact(200)), fastFramesBeforeClear(compiledNight, night));
}

TEST(AdaptiveRefreshPolicyConfig, ContentWeightsComeFromConfig) {
  Policy::Config config;
  config.weightImagePermille = 2000;  // image = 3.0x
  config.weightGrayscalePermille = 0;
  config.weightGrayscaleImageExtraPermille = 0;
  auto image = exact(100);
  image.hasImage = true;
  Policy p(false, config);
  (void)p.decide(image);
  EXPECT_EQ(p.pendingCharge(), 300u);

  auto grayText = exact(100);
  grayText.visibleGrayscale = true;
  (void)p.decide(grayText);
  EXPECT_EQ(p.pendingCharge(), 100u);  // grayscale weight 0: AA text costs like BW text
}

TEST(AdaptiveRefreshPolicyConfig, FrameChargeBillsEveryFastFrameRegardlessOfChange) {
  Policy::Config config;
  config.frameChargePermille = 100;  // 0.1 screen per frame
  Policy p(false, config);
  EXPECT_EQ(fastFramesBeforeClear(p, exact(0)), 14u);  // 15th frame reaches 1500
  Policy mixed(false, config);
  EXPECT_EQ(fastFramesBeforeClear(mixed, exact(200)), 4u);  // 300/frame
}

TEST(AdaptiveRefreshPolicyConfig, SafetyCapFromConfig) {
  Policy::Config config;
  config.safetyCap = 5;
  Policy p(false, config);
  for (unsigned i = 0; i < 5; ++i) ASSERT_EQ(step(p, exact(1)).refresh, Refresh::Fast);
  EXPECT_EQ(p.decide(exact(1)).reason, Reason::SafetyCap);
}

TEST(AdaptiveRefreshPolicyConfig, DefaultConfigReproducesTheV1RuleExactly) {
  // v1 charged changed * (4 + gray + 4*image + 2*grayImage) / 4. The permille
  // weights must floor to the same integer for every content mix and count.
  for (uint32_t changed = 0; changed <= 4000; changed += 7) {
    for (int mask = 0; mask < 4; ++mask) {
      const bool gray = mask & 1;
      const bool image = mask & 2;
      Policy::Inputs in;
      in.frame.pixelCount = 1000000;
      in.frame.frameValid = true;
      in.frame.historyValid = true;
      in.frame.changedPixels = changed;
      in.visibleGrayscale = gray;
      in.hasImage = image;
      const uint64_t quarters = 4 + (gray ? 1 : 0) + (image ? 4 : 0) + (gray && image ? 2 : 0);
      Policy p(false);
      (void)p.decide(in);
      ASSERT_EQ(p.pendingCharge(), changed * quarters / 4) << changed << " mask " << mask;
    }
  }
}

namespace {

// A reader page turn: changed pixels, AA text greys, and optionally an inline picture.
Policy::Inputs readerPage(uint32_t changed, bool picture) {
  auto in = exact(changed);
  in.visibleGrayscale = true;
  in.hasImage = picture;
  return in;
}

}  // namespace

TEST(AdaptiveRefreshPolicyImages, CompiledConfigKeepsTheClearAfterEveryPicture) {
  // rc9.2 called requestClear() after every picture page; Config{} must give the
  // same refresh, reason and debt on every page of a mixed book.
  Policy current;
  Policy rc92;
  uint32_t seed = 12345;
  unsigned pictures = 0;
  for (int i = 0; i < 600; ++i) {
    seed = seed * 1103515245u + 12345u;
    const uint32_t changed = 20 + (seed >> 8) % 600;
    const bool picture = (seed >> 20) % 7 == 0;
    const auto in = readerPage(changed, picture);
    const auto a = current.decide(in);
    const auto b = rc92.decide(in);
    ASSERT_EQ(a.refresh, b.refresh) << "page " << i;
    ASSERT_EQ(a.reason, b.reason) << "page " << i;
    current.commit(a.refresh);
    rc92.commit(b.refresh);
    if (picture) {
      ++pictures;
      current.imagePageShown();
      rc92.requestClear();
      ASSERT_TRUE(current.clearPending()) << "the page after a picture is a clear by default";
    }
    ASSERT_EQ(current.debtPixels(), rc92.debtPixels()) << "page " << i;
  }
  EXPECT_GT(pictures, 50u);
}

TEST(AdaptiveRefreshPolicyImages, WithoutClearAfterImageThePictureIsChargedNotCleared) {
  Policy::Config config;
  config.clearAfterImage = false;
  Policy p(false, config);
  const auto picture = step(p, readerPage(100, true));  // 100 * 2.75 = 275
  ASSERT_EQ(picture.refresh, Refresh::Fast);
  p.imagePageShown();
  EXPECT_FALSE(p.clearPending());
  EXPECT_EQ(p.debtPixels(), 275u) << "a fast picture page is charged once";
  // AA text pages cost 125 each: 275 + 125 * 10 reaches 1500 on the tenth.
  EXPECT_EQ(fastFramesBeforeClear(p, readerPage(100, false)), 9u);
  Policy textOnly(false, config);
  EXPECT_EQ(fastFramesBeforeClear(textOnly, readerPage(100, false)), 11u);
}

TEST(AdaptiveRefreshPolicyImages, APictureThatIsItselfTheClearOpensTheNextBudget) {
  Policy::Config config;
  config.clearAfterImage = false;
  Policy p(false, config);
  for (int i = 0; i < 11; ++i) ASSERT_EQ(step(p, readerPage(100, false)).refresh, Refresh::Fast);
  ASSERT_EQ(p.debtPixels(), 1375u);
  const auto picture = step(p, readerPage(200, true));  // 550 more crosses 1500
  ASSERT_EQ(picture.refresh, Refresh::Clear);
  ASSERT_EQ(picture.reason, Reason::Debt);
  EXPECT_EQ(p.debtPixels(), 0u);
  p.imagePageShown();
  EXPECT_EQ(p.debtPixels(), 550u) << "the picture's greys came after the clearing base";
  EXPECT_FALSE(p.clearPending());
  // 550 + 125 * 8 reaches 1500 on the eighth text page.
  EXPECT_EQ(fastFramesBeforeClear(p, readerPage(100, false)), 7u);
}

TEST(AdaptiveRefreshPolicyImages, PictureChargeFollowsTheImageWeights) {
  Policy::Config config;
  config.clearAfterImage = false;
  config.weightImagePermille = 0;
  config.weightGrayscaleImageExtraPermille = 0;
  Policy p(false, config);
  (void)step(p, readerPage(100, true));
  p.imagePageShown();
  EXPECT_EQ(p.debtPixels(), 125u) << "image weights 0: a picture page costs like an AA text page";
}

namespace {

using Rect = Policy::Rect;
using Fallback = Policy::RegionFallback;
using Source = Policy::ClearSource;

constexpr uint16_t kScreenW = 480;  // the X4 in portrait, logical pixels
constexpr uint16_t kScreenH = 800;

Policy::Config regionMode() {
  Policy::Config config;
  config.imageClearRegion = true;
  return config;
}

// A reader turn as EpubReaderActivity asks it: the windowed clear is prepared
// only when the policy wants it and the panel agrees (`panelReady`).
Policy::Inputs regionPage(const Policy& p, uint32_t changed, bool picture, bool panelReady = true) {
  auto in = readerPage(changed, picture);
  in.screenWidth = kScreenW;
  in.screenHeight = kScreenH;
  in.regionClearAvailable = p.regionClearWanted() && panelReady;
  return in;
}

struct Records {
  std::vector<Policy::ClearRecord> rows;
  static void observe(void* context, const Policy::ClearRecord& record) {
    static_cast<Records*>(context)->rows.push_back(record);
  }
};

// A picture page showing `pictures`, then the decision for the text page after it.
Policy::Decision afterPicture(Policy& p, const Rect pictures, const bool panelReady = true) {
  (void)step(p, regionPage(p, 100, true));
  p.imagePageShown(pictures);
  return p.decide(regionPage(p, 100, false, panelReady));
}

// The full-screen fallback of the picture clear, as the benchmark row reports it.
Fallback fallbackAfterPicture(const Rect pictures, const bool panelReady = true) {
  Policy p(false, regionMode());
  Records records;
  p.setClearObserver(&Records::observe, &records);
  const auto decision = afterPicture(p, pictures, panelReady);
  EXPECT_EQ(decision.refresh, Refresh::Clear);
  EXPECT_EQ(decision.reason, Reason::Requested);
  p.commit(decision.refresh);
  EXPECT_EQ(p.debtPixels(), 0u) << "a full clear ends the debt window";
  EXPECT_EQ(records.rows.size(), 1u);
  if (records.rows.empty()) return Fallback::None;
  EXPECT_FALSE(records.rows[0].regionOnly);
  return records.rows[0].regionFallback;
}

bool sameRecord(const Policy::ClearRecord& a, const Policy::ClearRecord& b) {
  return a.reason == b.reason && a.source == b.source && a.fastTurns == b.fastTurns && a.imagePages == b.imagePages &&
         a.grayPages == b.grayPages && a.debtPermille == b.debtPermille && a.thresholdPermille == b.thresholdPermille &&
         a.image == b.image && a.grayscale == b.grayscale && a.regionOnly == b.regionOnly &&
         a.regionFallback == b.regionFallback && a.region == b.region;
}

}  // namespace

TEST(AdaptiveRefreshPolicyRegion, TheClearAfterAPictureCoversOnlyThePicture) {
  Policy p(false, regionMode());
  Records records;
  p.setClearObserver(&Records::observe, &records);
  const Rect picture{40, 120, 400, 300};
  const auto decision = afterPicture(p, picture);
  EXPECT_EQ(decision.refresh, Refresh::Region);
  EXPECT_EQ(decision.reason, Reason::Requested);
  EXPECT_EQ(decision.region, picture);
  p.commit(decision.refresh);
  ASSERT_EQ(records.rows.size(), 1u);
  const auto& row = records.rows[0];
  EXPECT_TRUE(row.regionOnly);
  EXPECT_EQ(row.region, picture);
  EXPECT_EQ(row.regionFallback, Fallback::None);
  EXPECT_EQ(row.source, Source::Image);
  EXPECT_EQ(row.fastTurns, 1u) << "the picture page";
  EXPECT_EQ(row.imagePages, 1u);
  EXPECT_FALSE(p.clearPending()) << "the region clear settles the picture clear";
  EXPECT_FALSE(p.regionClearWanted());
  EXPECT_EQ(step(p, regionPage(p, 100, false)).refresh, Refresh::Fast);
}

TEST(AdaptiveRefreshPolicyRegion, OnlyThePictureClearIsOfferedAsARegion) {
  Policy fresh(true, regionMode());
  EXPECT_FALSE(fresh.regionClearWanted()) << "the first page clears the whole screen";

  Policy p(false, regionMode());
  EXPECT_FALSE(p.regionClearWanted()) << "nothing pending";
  (void)step(p, regionPage(p, 100, true));
  p.imagePageShown({0, 0, 100, 100});
  EXPECT_TRUE(p.regionClearWanted());
  p.requestClear(Source::Popup);
  EXPECT_FALSE(p.regionClearWanted()) << "another reader request needs the whole screen";
  EXPECT_EQ(p.decide(regionPage(p, 100, false)).refresh, Refresh::Clear);

  Policy manual(false, regionMode());
  (void)step(manual, regionPage(manual, 100, true));
  manual.imagePageShown({0, 0, 100, 100});
  manual.requestClear(Source::Manual);
  auto in = regionPage(manual, 100, false);
  in.regionClearAvailable = true;  // even if the caller offers one
  EXPECT_EQ(manual.decide(in).refresh, Refresh::Clear);

  Policy compiled(false);  // region mode off
  (void)step(compiled, regionPage(compiled, 100, true));
  compiled.imagePageShown({0, 0, 100, 100});
  EXPECT_FALSE(compiled.regionClearWanted());
  in = regionPage(compiled, 100, false);
  in.regionClearAvailable = true;
  EXPECT_EQ(compiled.decide(in).refresh, Refresh::Clear);
}

TEST(AdaptiveRefreshPolicyRegion, ARegionClearLeavesTheTextDebtWindowOpen) {
  Policy p(false, regionMode());
  for (int i = 0; i < 4; ++i) ASSERT_EQ(step(p, regionPage(p, 100, false)).refresh, Refresh::Fast);
  ASSERT_EQ(p.debtPixels(), 500u);  // 4 AA text pages * 125
  const auto decision = afterPicture(p, {40, 120, 400, 300});
  ASSERT_EQ(decision.refresh, Refresh::Region);
  p.commit(decision.refresh);
  EXPECT_EQ(p.debtPixels(), 900u) << "500 + the picture page (275) + this text page (125)";
  EXPECT_EQ(p.fastSinceClear(), 6u);
  // 900 + 125 * 5 reaches 1500 on the fifth page: four fast turns, then the debt clear.
  EXPECT_EQ(fastFramesBeforeClear(p, readerPage(100, false)), 4u);
}

TEST(AdaptiveRefreshPolicyRegion, PicturesShownBeforeTheClearRunsAreUnited) {
  Policy p(false, regionMode());
  (void)step(p, regionPage(p, 100, true));
  p.imagePageShown({40, 100, 200, 150});
  p.imagePageShown({120, 400, 300, 100});  // a second picture page before the clear ran
  EXPECT_EQ(p.decide(regionPage(p, 100, false)).region, (Rect{40, 100, 380, 400}));

  // Consecutive picture pages: each page clears its predecessor's pictures only.
  Policy book(false, regionMode());
  const Rect first{0, 0, 480, 300};
  const Rect second{40, 500, 400, 200};
  (void)step(book, regionPage(book, 100, true));
  book.imagePageShown(first);
  const auto onSecond = step(book, regionPage(book, 100, true));
  ASSERT_EQ(onSecond.refresh, Refresh::Region);
  EXPECT_EQ(onSecond.region, first);
  book.imagePageShown(second);
  EXPECT_EQ(book.decide(regionPage(book, 100, false)).region, second) << "the clear that ran forgets the first";
}

TEST(AdaptiveRefreshPolicyRegion, TheAreaIsClampedToTheScreen) {
  Policy p(false, regionMode());
  EXPECT_EQ(afterPicture(p, {-16, 700, 200, 200}).region, (Rect{0, 700, 184, 100}));
  EXPECT_EQ(fallbackAfterPicture({480, 0, 100, 100}), Fallback::OffScreen);
  EXPECT_EQ(fallbackAfterPicture({-200, 10, 200, 100}), Fallback::OffScreen);
}

TEST(AdaptiveRefreshPolicyRegion, PicturesOverSixtyPercentOfTheScreenClearItAll) {
  Policy p(false, regionMode());
  const Rect sixty{0, 0, 480, 480};  // 230400 of 384000 pixels: exactly 60 %
  const auto decision = afterPicture(p, sixty);
  EXPECT_EQ(decision.refresh, Refresh::Region);
  EXPECT_EQ(decision.region, sixty);
  EXPECT_EQ(fallbackAfterPicture({0, 0, 480, 481}), Fallback::TooLarge);
  EXPECT_EQ(fallbackAfterPicture({0, 0, 480, 800}), Fallback::TooLarge);
  // Two small pictures far apart unite into one tall rect.
  Policy spread(false, regionMode());
  (void)step(spread, regionPage(spread, 100, true));
  spread.imagePageShown({0, 0, 480, 100});
  spread.imagePageShown({0, 700, 480, 100});
  EXPECT_EQ(spread.decide(regionPage(spread, 100, false)).refresh, Refresh::Clear);
}

TEST(AdaptiveRefreshPolicyRegion, AnUnknownOrUnavailableRegionClearsTheWholeScreen) {
  EXPECT_EQ(fallbackAfterPicture({}), Fallback::Unknown);
  EXPECT_EQ(fallbackAfterPicture({40, 120, 400, 300}, /*panelReady=*/false), Fallback::Unavailable);

  Policy p(false, regionMode());
  (void)step(p, regionPage(p, 100, true));
  p.imagePageShown({40, 120, 100, 100});
  p.imagePageShown({});  // one page did not say where its pictures were
  EXPECT_EQ(p.decide(regionPage(p, 100, false)).refresh, Refresh::Clear);
}

TEST(AdaptiveRefreshPolicyRegion, StrongerReasonsStillClearTheWholeScreen) {
  // Debt: 9 text pages (1125) + the picture page (275) + the next page (125) reach 1500.
  Policy debt(false, regionMode());
  Records records;
  debt.setClearObserver(&Records::observe, &records);
  for (int i = 0; i < 9; ++i) ASSERT_EQ(step(debt, regionPage(debt, 100, false)).refresh, Refresh::Fast);
  const auto atDebt = afterPicture(debt, {40, 120, 100, 100});
  EXPECT_EQ(atDebt.refresh, Refresh::Clear);
  debt.commit(atDebt.refresh);
  ASSERT_EQ(records.rows.size(), 1u);
  EXPECT_EQ(records.rows[0].regionFallback, Fallback::Overruled);
  EXPECT_EQ(debt.debtPixels(), 0u);

  // Safety cap: the third fast frame in a row reaches a cap of 3.
  auto capped = regionMode();
  capped.safetyCap = 3;
  Policy cap(false, capped);
  ASSERT_EQ(step(cap, regionPage(cap, 10, false)).refresh, Refresh::Fast);
  ASSERT_EQ(step(cap, regionPage(cap, 10, false)).refresh, Refresh::Fast);
  EXPECT_EQ(afterPicture(cap, {40, 120, 100, 100}).refresh, Refresh::Clear);

  // A renderer promotion (popup close, manual refresh tile) already queued.
  Policy promoted(false, regionMode());
  (void)step(promoted, regionPage(promoted, 100, true));
  promoted.imagePageShown({40, 120, 100, 100});
  auto in = regionPage(promoted, 100, false);
  in.externalClearPending = true;
  EXPECT_EQ(promoted.decide(in).refresh, Refresh::Clear);

  // Night-mode polarity flip and unreadable frames rewrite everything.
  Policy flipped(false, regionMode());
  (void)step(flipped, regionPage(flipped, 100, true));
  flipped.imagePageShown({40, 120, 100, 100});
  in = regionPage(flipped, 100, false);
  in.frame.polarityChangePending = true;
  EXPECT_EQ(flipped.decide(in).refresh, Refresh::Clear);
  in = regionPage(flipped, 100, false);
  in.frame.frameValid = false;
  EXPECT_EQ(flipped.decide(in).refresh, Refresh::Clear);
}

TEST(AdaptiveRefreshPolicyRegion, RegionModeOffIsTheRc922RuleExactly) {
  // The key off (compiled) must give rc9.2.2's decisions, reasons, debt and
  // clear rows on every page, whatever the reader now passes in. With
  // clear_after_image off the region key has nothing to act on.
  Policy::Config noClearAfter;
  noClearAfter.clearAfterImage = false;
  Policy::Config noClearAfterRegion = noClearAfter;
  noClearAfterRegion.imageClearRegion = true;
  const Policy::Config pairs[][2] = {{Policy::Config{}, Policy::Config{}}, {noClearAfter, noClearAfterRegion}};
  for (const auto& pair : pairs) {
    Policy rc922(true, pair[0]);
    Policy candidate(true, pair[1]);
    Records rc922Rows;
    Records candidateRows;
    rc922.setClearObserver(&Records::observe, &rc922Rows);
    candidate.setClearObserver(&Records::observe, &candidateRows);
    uint32_t seed = 777;
    for (int i = 0; i < 600; ++i) {
      seed = seed * 1103515245u + 12345u;
      const uint32_t changed = 20 + (seed >> 8) % 600;
      const bool picture = (seed >> 20) % 5 == 0;
      const auto plain = readerPage(changed, picture);
      auto rich = regionPage(candidate, changed, picture);
      rich.regionClearAvailable = true;  // a reader that always offers a window
      const auto a = rc922.decide(plain);
      const auto b = candidate.decide(rich);
      ASSERT_EQ(a.refresh, b.refresh) << "page " << i;
      ASSERT_EQ(a.reason, b.reason) << "page " << i;
      rc922.commit(a.refresh);
      candidate.commit(b.refresh);
      if (picture) {
        rc922.imagePageShown();
        candidate.imagePageShown({static_cast<int16_t>(seed % 200), static_cast<int16_t>(seed % 500), 200, 200});
      }
      ASSERT_EQ(rc922.debtPixels(), candidate.debtPixels()) << "page " << i;
      ASSERT_EQ(rc922.clearPending(), candidate.clearPending()) << "page " << i;
    }
    ASSERT_EQ(rc922Rows.rows.size(), candidateRows.rows.size());
    EXPECT_GT(rc922Rows.rows.size(), 20u);
    for (size_t i = 0; i < rc922Rows.rows.size(); ++i) {
      EXPECT_TRUE(sameRecord(rc922Rows.rows[i], candidateRows.rows[i])) << "clear " << i;
    }
  }
}

namespace {

Policy::Inputs dwell(uint32_t changed, uint32_t elapsedMs) {
  auto in = exact(changed);
  in.elapsedMs = elapsedMs;
  return in;
}

// The control center's refresh tile: a renderer promotion labelled manual.
void manualClear(Policy& p) {
  auto in = exact(100);
  in.externalClearPending = true;
  in.externalManual = true;
  const auto d = p.decide(in);
  ASSERT_EQ(d.refresh, Refresh::Clear);
  p.commit(d.refresh);
}

// Fast frames before a clear when every frame changes `changed` pixels laid
// out on the 8x6 tile plane as `tiles`.
unsigned fastTilesBeforeClear(Policy& p, const uint16_t (&tiles)[Policy::kTileCount], uint32_t changed,
                              Reason* reason = nullptr, unsigned limit = 500) {
  for (unsigned n = 0; n < limit; ++n) {
    p.recordTileTransitions(tiles, 0);
    const auto d = step(p, exact(changed));
    if (d.refresh == Refresh::Clear) {
      if (reason) *reason = d.reason;
      return n;
    }
  }
  return limit;
}

// Rounds of `turns` fast pages, each ended by a manual refresh.
void annoy(Policy& p, unsigned rounds, unsigned turns = 6) {
  for (unsigned r = 0; r < rounds; ++r) {
    for (unsigned i = 0; i < turns; ++i) ASSERT_EQ(step(p, exact(100)).refresh, Refresh::Fast) << r << "/" << i;
    manualClear(p);
    ASSERT_TRUE(p.recordManualRefresh()) << "round " << r;
  }
}

}  // namespace

TEST(LiquidRefresh, RhoFollowsTheClosedFormDecay) {
  for (const bool inverted : {false, true}) {
    for (uint8_t cls = Policy::kLiquidText; cls <= Policy::kLiquidImage; ++cls) {
      EXPECT_EQ(Policy::liquidRhoPermille(0, cls, inverted), 1000);
      const int32_t logit = Policy::kLiquidLogitMilli[cls] + (inverted ? Policy::kLiquidLogitInvertedMilli : 0);
      const double rate = std::log1p(std::exp(logit / 1000.0));  // softplus, per second
      uint16_t previous = 1000;
      for (const uint32_t ms : {50u, 300u, 1000u, 5000u, 20000u, 60000u, 600000u, 4000000000u}) {
        const uint16_t rho = Policy::liquidRhoPermille(ms, cls, inverted);
        EXPECT_NEAR(rho, 1000.0 * std::exp(-(ms / 1000.0) * rate), 2.0) << int(cls) << " " << inverted << " " << ms;
        EXPECT_LE(rho, previous);
        previous = rho;
      }
    }
  }
  // Night mode and heavier content relax more slowly.
  EXPECT_GT(Policy::liquidRhoPermille(20000, Policy::kLiquidText, true),
            Policy::liquidRhoPermille(20000, Policy::kLiquidText, false));
  EXPECT_GT(Policy::liquidRhoPermille(20000, Policy::kLiquidImage, false),
            Policy::liquidRhoPermille(20000, Policy::kLiquidText, false));
}

TEST(LiquidRefresh, UniformTilesWithoutDwellAreTheContentV2RuleExactly) {
  uint16_t uniform[Policy::kTileCount];
  Policy legacy;
  Policy liquid;
  uint32_t seed = 4242;
  for (int i = 0; i < 600; ++i) {
    seed = seed * 1103515245u + 12345u;
    const uint32_t changed = 20 + (seed >> 8) % 600;
    const bool picture = (seed >> 20) % 7 == 0;
    for (auto& t : uniform) t = static_cast<uint16_t>(changed);  // kPixels = 1000: changed is permille
    liquid.recordTileTransitions(uniform, 0);
    const auto a = legacy.decide(readerPage(changed, picture));
    const auto b = liquid.decide(readerPage(changed, picture));
    ASSERT_EQ(a.refresh, b.refresh) << "page " << i;
    ASSERT_EQ(a.reason, b.reason) << "page " << i;
    legacy.commit(a.refresh);
    liquid.commit(b.refresh);
    if (picture) {
      legacy.imagePageShown();
      liquid.imagePageShown();
    }
    ASSERT_EQ(legacy.debtPixels(), liquid.debtPixels()) << "page " << i;
  }
}

TEST(LiquidRefresh, DwellRelaxesDebtWhileRapidTurnsChargeMore) {
  Policy legacy(false);
  EXPECT_EQ(fastFramesBeforeClear(legacy, dwell(100, 0)), 14u);

  // A minute per page: the relaxing half of each charge fades, the
  // irreversible half still reaches the budget well before the safety cap.
  Policy slow(false);
  unsigned slowTurns = 0;
  Policy::Decision d{};
  while ((d = step(slow, dwell(100, 60000))).refresh == Refresh::Fast) ++slowTurns;
  EXPECT_EQ(d.reason, Reason::Debt);
  EXPECT_GT(slowTurns, 20u);
  EXPECT_LT(slowTurns, static_cast<unsigned>(Policy::kSafetyCap));

  // Flipping every 300 ms: almost no relaxation and each turn is boosted.
  Policy rapid(false);
  EXPECT_LT(fastFramesBeforeClear(rapid, dwell(100, 300)), 14u);

  // The debt after one long dwell sits between the floor and the full debt.
  Policy one(false);
  step(one, dwell(400, 0));
  ASSERT_EQ(one.debtPixels(), 400u);
  step(one, dwell(0, 60000));
  EXPECT_GE(one.debtPixels(), 200u);
  EXPECT_LT(one.debtPixels(), 230u);
}

TEST(LiquidRefresh, PeakTileClearsEarlierThanTheSameChangeSpreadOut) {
  // Both layouts change 100 of 1000 pixels (100 permille of the screen).
  uint16_t uniform[Policy::kTileCount];
  for (auto& t : uniform) t = 100;
  uint16_t peak[Policy::kTileCount] = {};
  for (int i = 0; i < 5; ++i) peak[i] = 960;  // 5 * 960 / 48 = 100

  Policy spread(false);
  Reason spreadReason = Reason::None;
  EXPECT_EQ(fastTilesBeforeClear(spread, uniform, 100, &spreadReason), 14u);
  EXPECT_EQ(spreadReason, Reason::Debt);

  Policy local(false);
  Reason localReason = Reason::None;
  const unsigned localTurns = fastTilesBeforeClear(local, peak, 100, &localReason);
  EXPECT_LE(localTurns, 3u);
  EXPECT_EQ(localReason, Reason::Debt);

  // The clear wipes the tile plane; one more fast frame books the tile charge.
  EXPECT_EQ(local.tileGhostPermille(0), 0);
  local.recordTileTransitions(peak, 0);
  ASSERT_EQ(step(local, exact(100)).refresh, Refresh::Fast);
  EXPECT_EQ(local.tileGhostPermille(0), 960);
  EXPECT_EQ(local.tileGhostPermille(47), 0);
}

TEST(LiquidRefresh, ManualRefreshesLearnEarlierClearsThatPersist) {
  Policy learner(false);
  annoy(learner, 6);
  EXPECT_EQ(learner.liquidUpdates(), 6u);
  EXPECT_FALSE(learner.recordManualRefresh());  // the trace was consumed
  const uint32_t learned = learner.learnedThresholdPermille();
  EXPECT_LT(learned, Policy::kClearThresholdPermille);
  EXPECT_GE(learned, Policy::kClearThresholdPermille / 2);

  uint8_t buf[Policy::kLearnedStateBytes + 4];
  EXPECT_EQ(learner.saveLearnedState(buf, Policy::kLearnedStateBytes - 1), 0u);
  ASSERT_EQ(learner.saveLearnedState(buf, sizeof buf), Policy::kLearnedStateBytes);
  EXPECT_EQ(std::memcmp(buf, "LQRF", 4), 0);

  Policy restored(false);
  ASSERT_TRUE(restored.loadLearnedState(buf, Policy::kLearnedStateBytes));
  EXPECT_EQ(restored.liquidReadout(), learner.liquidReadout());
  EXPECT_EQ(restored.liquidUpdates(), 6u);
  EXPECT_EQ(restored.learnedThresholdPermille(), learned);
  restored.reset();  // a new book keeps what was learned
  EXPECT_EQ(restored.liquidReadout(), learner.liquidReadout());

  Policy fresh(false);
  restored.reset(false);
  const unsigned learnedTurns = fastFramesBeforeClear(restored, exact(100));
  EXPECT_LT(learnedTurns, fastFramesBeforeClear(fresh, exact(100)));
  EXPECT_GE(learnedTurns, 7u);  // never below half the configured threshold
}

TEST(LiquidRefresh, LearningStaysWithinHalfAndOneAndAHalfThresholdsAndTheSafetyCap) {
  Policy p(false);
  annoy(p, 200);
  EXPECT_GE(p.learnedThresholdPermille(), Policy::kClearThresholdPermille / 2);
  EXPECT_LE(p.learnedThresholdPermille(), Policy::kClearThresholdPermille * 3 / 2);
  EXPECT_GE(fastFramesBeforeClear(p, exact(100)), 7u);

  // Tiny changes still clear at the safety cap, not earlier or later.
  Policy capped(false);
  uint8_t buf[Policy::kLearnedStateBytes];
  ASSERT_EQ(p.saveLearnedState(buf, sizeof buf), Policy::kLearnedStateBytes);
  ASSERT_TRUE(capped.loadLearnedState(buf, sizeof buf));
  for (unsigned i = 0; i < Policy::kSafetyCap; ++i) ASSERT_EQ(step(capped, exact(1)).refresh, Refresh::Fast) << i;
  EXPECT_EQ(capped.decide(exact(1)).reason, Reason::SafetyCap);
}

TEST(LiquidRefresh, PolicyClearsEndTheTraceAndBadStateIsRejected) {
  Policy p(false);
  for (int i = 0; i < 6; ++i) step(p, exact(100));
  p.requestClear();
  ASSERT_EQ(step(p, exact(100)).reason, Reason::Requested);
  EXPECT_FALSE(p.recordManualRefresh());  // no complaint pending after a policy clear

  // Recorded before the manual clear is committed works too.
  for (int i = 0; i < 6; ++i) step(p, exact(100));
  EXPECT_TRUE(p.recordManualRefresh());
  const auto readout = p.liquidReadout();
  EXPECT_NE(readout, Policy::LiquidReadout{});

  uint8_t buf[Policy::kLearnedStateBytes];
  ASSERT_EQ(p.saveLearnedState(buf, sizeof buf), Policy::kLearnedStateBytes);
  Policy other(false);
  EXPECT_FALSE(other.loadLearnedState(buf, sizeof buf - 1));
  buf[9] ^= 0x40;  // corrupt v0
  EXPECT_FALSE(other.loadLearnedState(buf, sizeof buf));
  EXPECT_EQ(other.liquidReadout(), Policy::LiquidReadout{});
  buf[9] ^= 0x40;
  ASSERT_TRUE(other.loadLearnedState(buf, sizeof buf));
  EXPECT_EQ(other.liquidReadout(), readout);
  other.resetLearnedState();
  EXPECT_EQ(other.liquidReadout(), Policy::LiquidReadout{});
}

namespace {

// The card's policy file (refresh-policy.json): what the CfC was fitted under.
Policy::Config cardConfig() {
  Policy::Config c;
  c.clearThresholdPermille = 10000;
  c.nightClearThresholdPermille = 8000;
  c.safetyCap = 48;
  c.weightGrayscalePermille = c.weightImagePermille = c.weightGrayscaleImageExtraPermille = 0;
  c.frameChargePermille = 0;
  c.clearAfterImage = false;
  return c;
}

uint32_t fnv1a(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  while (n--) h = (h ^ *p++) * 16777619u;
  return h;
}

// A text page after a recorded image: refresh, reason and y0 for `dwellMs`.
Policy::Decision textAfterImage(uint32_t dwellMs, uint16_t areaPermille, uint64_t tiles = 0,
                                const uint16_t* changedTiles = nullptr) {
  Policy p(false, cardConfig());
  EXPECT_EQ(step(p, dwell(300, 30000)).refresh, Refresh::Fast);
  p.recordImageExposure(dwellMs, areaPermille, 600, tiles);
  if (changedTiles) p.recordTileTransitions(changedTiles, 0);
  return step(p, dwell(300, dwellMs + 400));
}

}  // namespace

TEST(LiquidCfc, TextChargeRisesWithRapidTurnsAndRelaxesOverAReadingDwell) {
  Policy p(false, cardConfig());
  EXPECT_EQ(p.cfcChargeQ8(), 0);
  int16_t previous = 0;
  for (int i = 0; i < 6; ++i) {
    ASSERT_EQ(step(p, dwell(400, 600)).refresh, Refresh::Fast) << i;
    EXPECT_GT(p.cfcChargeQ8(), previous) << "turn " << i;
    previous = p.cfcChargeQ8();
  }
  EXPECT_GT(previous, 128) << "six 0.6 s turns charge the cell past half scale";
  step(p, dwell(400, 30000));
  EXPECT_LT(p.cfcChargeQ8(), previous / 2) << "a 30 s read relaxes the charge";
  for (uint8_t u = 0; u < Policy::kCfcUnits; ++u) {
    EXPECT_GE(p.cfcHiddenQ8(u), -256);
    EXPECT_LE(p.cfcHiddenQ8(u), 256);
  }
}

TEST(LiquidCfc, ImageStickingGrowsWithViewingDwellAndRelaxesUnderText) {
  int16_t previous = -1;
  for (const uint32_t ms : {500u, 2000u, 4000u, 8000u, 16000u, 40000u}) {
    Policy p(false, cardConfig());
    p.recordImageExposure(ms, 500, 600);
    EXPECT_GT(p.cfcStickQ8(), previous) << ms;
    previous = p.cfcStickQ8();
    EXPECT_NE(p.imageTileMask(), 0u) << "an unknown footprint covers every tile";
  }
  EXPECT_GT(previous, 64);

  Policy p(false, cardConfig());
  p.recordImageExposure(15000, 60, 600);
  int16_t stick = p.cfcStickQ8();
  EXPECT_GT(stick, 0);
  for (int i = 0; i < 4; ++i) {
    ASSERT_EQ(step(p, dwell(300, 30000)).refresh, Refresh::Fast) << "a 6% picture does not force a clear";
    EXPECT_LT(p.cfcStickQ8(), stick) << "text page " << i;
    stick = p.cfcStickQ8();
  }
}

TEST(LiquidCfc, ALargeLongViewedImageClearsTheTextPageThatOverwritesIt) {
  for (const uint32_t ms : {4000u, 8000u, 16000u, 40000u}) {
    for (const uint16_t area : {250, 400, 900}) {
      const auto d = textAfterImage(ms, area);
      EXPECT_EQ(d.refresh, Refresh::Clear) << ms << " ms, " << area << " permille";
      EXPECT_EQ(d.reason, Reason::ImageSticking) << ms << " ms, " << area << " permille";
    }
    for (const uint16_t area : {20, 50}) {
      const auto d = textAfterImage(ms, area);
      EXPECT_EQ(d.refresh, Refresh::Fast) << ms << " ms, " << area << " permille";
    }
  }

  // The clear ends the episode: the next text page is fast again.
  Policy p(false, cardConfig());
  p.recordImageExposure(20000, 700, 800);
  const auto cleared = step(p, dwell(300, 20400));
  ASSERT_EQ(cleared.reason, Reason::ImageSticking);
  EXPECT_GE(p.cfcRefreshLogit(), 0);
  EXPECT_EQ(p.imageTileMask(), 0u);
  EXPECT_EQ(step(p, dwell(300, 30000)).refresh, Refresh::Fast);
}

TEST(LiquidCfc, OnlyTextChangingTilesThePictureCoveredCanStickClear) {
  EXPECT_EQ(Policy::tileMaskFor({0, 0, 100, 80}, 800, 480), 1u);
  EXPECT_EQ(Policy::tileMaskFor({700, 400, 100, 80}, 800, 480), uint64_t{1} << 47);
  EXPECT_EQ(Policy::tileMaskFor({0, 0, 800, 480}, 800, 480), ~uint64_t{0} >> (64 - Policy::kTileCount));
  EXPECT_EQ(Policy::tileMaskFor({0, 0, 0, 80}, 800, 480), 0u);

  // A picture in the top-left quarter; the next page changes text elsewhere.
  const uint64_t topLeft = Policy::tileMaskFor({0, 0, 400, 240}, 800, 480);
  uint16_t elsewhere[Policy::kTileCount];
  for (uint8_t i = 0; i < Policy::kTileCount; ++i) elsewhere[i] = (topLeft >> i) & 1 ? 0 : 300;
  EXPECT_EQ(textAfterImage(40000, 250, topLeft, elsewhere).refresh, Refresh::Fast);
  uint16_t over[Policy::kTileCount];
  for (auto& t : over) t = 300;
  const auto d = textAfterImage(40000, 250, topLeft, over);
  EXPECT_EQ(d.refresh, Refresh::Clear);
  EXPECT_EQ(d.reason, Reason::ImageSticking);
}

TEST(LiquidCfc, DitherHeadTightensBleedAndOpensContrastOnLineDiagrams) {
  Policy p(false, cardConfig());
  const auto photo = p.computeImageDitherParams(0, 800);
  const auto diagram = p.computeImageDitherParams(250, 100);
  EXPECT_LT(diagram.errorBleedQ8, photo.errorBleedQ8);
  EXPECT_GT(diagram.contrastGainQ8, photo.contrastGainQ8);
  EXPECT_GE(photo.errorBleedQ8, 240) << "continuous-tone photos keep (nearly) full diffusion";
  EXPECT_LE(diagram.errorBleedQ8, 200);
  for (const auto& d : {photo, diagram}) {
    EXPECT_GE(d.contrastGainQ8, Policy::kContrastGainMinQ8);
    EXPECT_LE(d.contrastGainQ8, Policy::kContrastGainMaxQ8);
    EXPECT_GE(d.errorBleedQ8, Policy::kErrorBleedMinQ8);
    EXPECT_LE(d.errorBleedQ8, Policy::kErrorBleedMaxQ8);
  }
  // Edge density saturates at kCfcEdgeFullPermille, and the query is pure.
  EXPECT_EQ(p.computeImageDitherParams(Policy::kCfcEdgeFullPermille, 100), p.computeImageDitherParams(1000, 100));
  EXPECT_EQ(p.computeImageDitherParams(0, 800), photo);
  EXPECT_EQ(p.cfcStickQ8(), 0);

  // Panel gray debt from a long image view opens the contrast further.
  Policy worn(false, cardConfig());
  worn.recordImageExposure(20000, 700, 800);
  EXPECT_GT(worn.computeImageDitherParams(120, 100).contrastGainQ8, p.computeImageDitherParams(120, 100).contrastGainQ8);
}

TEST(LiquidCfc, EpropLearnsTheGateReadoutAndLqrfV2KeepsIt) {
  Policy p(false);
  const auto fitted = p.cfcReadout();
  EXPECT_EQ(fitted, Policy::cfcFittedReadout());
  for (int i = 0; i < 6; ++i) ASSERT_EQ(step(p, dwell(100, 600)).refresh, Refresh::Fast) << i;
  bool traced = false;
  for (uint8_t u = 0; u < Policy::kCfcUnits; ++u) traced |= p.cfcTraceQ8(u) != 0;
  EXPECT_TRUE(traced);
  manualClear(p);
  ASSERT_TRUE(p.recordManualRefresh());
  EXPECT_EQ(p.cfcUpdates(), 1u);
  EXPECT_NE(p.cfcReadout(), fitted);
  EXPECT_GT(p.cfcReadout().b, fitted.b) << "a late clear pushes the gate's bias open";
  for (uint8_t u = 0; u < Policy::kCfcUnits; ++u) EXPECT_EQ(p.cfcTraceQ8(u), 0) << "the trace was consumed";

  // Many complaints: every weight stays within the drift bound of the fit.
  annoy(p, 200);
  const auto learned = p.cfcReadout();
  for (uint8_t u = 0; u < Policy::kCfcUnits; ++u) {
    EXPECT_LE(std::abs(learned.w[u] - fitted.w[u]), Policy::kCfcReadoutDrift) << int(u);
  }
  EXPECT_LE(std::abs(learned.b - fitted.b), Policy::kCfcReadoutDrift);

  uint8_t buf[Policy::kLearnedStateBytes];
  ASSERT_EQ(Policy::kLearnedStateBytes, 64u);
  ASSERT_EQ(p.saveLearnedState(buf, sizeof buf), Policy::kLearnedStateBytes);
  Policy restored(false);
  ASSERT_TRUE(restored.loadLearnedState(buf, sizeof buf));
  EXPECT_EQ(restored.cfcReadout(), learned);
  EXPECT_EQ(restored.cfcUpdates(), p.cfcUpdates());
  EXPECT_EQ(restored.liquidReadout(), p.liquidReadout());
  restored.reset();  // a new book keeps what was learned
  EXPECT_EQ(restored.cfcReadout(), learned);
  restored.resetLearnedState();
  EXPECT_EQ(restored.cfcReadout(), fitted);
  EXPECT_EQ(restored.cfcUpdates(), 0u);

  // A v1 (rc10 LiquidRefresh) 32-byte file still loads: its v1 readout, the
  // fitted CfC readout.
  uint8_t v1[Policy::kLearnedStateV1Bytes];
  std::memcpy(v1, buf, 28);
  const uint16_t version = 1;
  const uint16_t bytes = Policy::kLearnedStateV1Bytes;
  std::memcpy(v1 + 4, &version, 2);
  std::memcpy(v1 + 6, &bytes, 2);
  const uint32_t check = fnv1a(v1, 28);
  std::memcpy(v1 + 28, &check, 4);
  Policy upgraded(false);
  ASSERT_TRUE(upgraded.loadLearnedState(v1, sizeof v1));
  EXPECT_EQ(upgraded.liquidReadout(), p.liquidReadout());
  EXPECT_EQ(upgraded.liquidUpdates(), p.liquidUpdates());
  EXPECT_EQ(upgraded.cfcReadout(), fitted);
  EXPECT_EQ(upgraded.cfcUpdates(), 0u);
  v1[10] ^= 0x01;
  EXPECT_FALSE(Policy(false).loadLearnedState(v1, sizeof v1));

  // A v2 header cut to the v1 size is rejected.
  EXPECT_FALSE(Policy(false).loadLearnedState(buf, Policy::kLearnedStateV1Bytes));
}
