#include <gtest/gtest.h>

#include <cstdint>
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
