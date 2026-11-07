#include <Epub/Epub/PagePicture.h>
#include <Epub/Epub/converters/SmoothResample.h>
#include <gtest/gtest.h>

#include <vector>

namespace {

PagePicture picture(const int16_t x, const int16_t y, const int16_t width, const int16_t height,
                    const uint16_t element = 0) {
  PagePicture p;
  p.x = x;
  p.y = y;
  p.width = width;
  p.height = height;
  p.element = element;
  return p;
}

}  // namespace

TEST(PagePictureHitTest, MapsScreenPointThroughPageMargins) {
  // A picture at page (20, 100) 200x150 with a 16/40 left/top margin covers
  // screen x [36, 236) and y [140, 290).
  const std::vector<PagePicture> pictures{picture(20, 100, 200, 150)};
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 36, 140, 16, 40), 0);
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 235, 289, 16, 40), 0);
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 130, 200, 16, 40), 0);
}

TEST(PagePictureHitTest, TapsJustOffThePictureStillOpenIt) {
  const std::vector<PagePicture> pictures{picture(20, 100, 200, 150)};
  // Screen box [36, 236) x [140, 290) grows by TAP_PAD = 8 for finger slop.
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 28, 200, 16, 40), 0);
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 243, 200, 16, 40), 0);
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 130, 132, 16, 40), 0);
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 130, 297, 16, 40), 0);
  // One pixel past the pad: these stay page-turn taps.
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 27, 200, 16, 40), -1);
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 244, 200, 16, 40), -1);
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 130, 131, 16, 40), -1);
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 130, 298, 16, 40), -1);
  // Unshifted page coordinates would hit; the margins must be applied.
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 25, 110, 0, 0), 0);
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 25, 110, 16, 40), -1);
}

TEST(PagePictureHitTest, NoPicturesMeansNoHit) {
  const std::vector<PagePicture> none;
  EXPECT_EQ(page_picture::pictureAtPoint(none, 100, 100, 0, 0), -1);
}

TEST(PagePictureHitTest, SmallOrnamentsAreNotTapTargets) {
  // Drop cap 40x40 and a 400x6 rule image: taps on them still turn pages.
  const std::vector<PagePicture> pictures{picture(0, 0, 40, 40), picture(0, 100, 400, 6), picture(0, 200, 48, 48)};
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 10, 10, 0, 0), -1);
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 200, 102, 0, 0), -1);
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 10, 210, 0, 0), 2);
}

TEST(PagePictureHitTest, ReturnsThePictureUnderTheFinger) {
  const std::vector<PagePicture> pictures{picture(0, 0, 440, 200, 3), picture(0, 260, 440, 300, 7)};
  const int top = page_picture::pictureAtPoint(pictures, 100, 100, 0, 0);
  const int bottom = page_picture::pictureAtPoint(pictures, 100, 400, 0, 0);
  ASSERT_EQ(top, 0);
  ASSERT_EQ(bottom, 1);
  EXPECT_EQ(pictures[bottom].element, 7);
  // The gap between the two pictures is text.
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 100, 230, 0, 0), -1);
}

TEST(PagePictureSource, RefusesWhatThePageDecodersRefuse) {
  using page_picture::SourceCheck;
  EXPECT_EQ(page_picture::checkSource(2000, 3000), SourceCheck::Ok);
  EXPECT_EQ(page_picture::checkSource(2048, 4096), SourceCheck::Ok);  // exactly 8 MP
  EXPECT_EQ(page_picture::checkSource(2048, 4097), SourceCheck::TooLarge);
  EXPECT_EQ(page_picture::checkSource(40000, 10), SourceCheck::TooLarge);
  EXPECT_EQ(page_picture::checkSource(10, 40000), SourceCheck::TooLarge);
  EXPECT_EQ(page_picture::checkSource(0, 100), SourceCheck::Invalid);
  EXPECT_EQ(page_picture::checkSource(100, -1), SourceCheck::Invalid);
}

TEST(PagePictureFit, PortraitPictureFillsPortraitScreenInsideMargin) {
  const auto fit = page_picture::fitToScreen(1000, 1500, 480, 800);
  EXPECT_FALSE(fit.rotate);
  EXPECT_EQ(fit.width, 472);
  EXPECT_EQ(fit.height, 708);
  EXPECT_EQ(fit.x, 4);
  EXPECT_EQ(fit.y, 46);
}

TEST(PagePictureFit, SmallPictureUpscaleIsCappedAtThreeTimes) {
  const auto fit = page_picture::fitToScreen(120, 160, 480, 800);
  EXPECT_FALSE(fit.rotate);
  EXPECT_EQ(fit.width, 360);
  EXPECT_EQ(fit.height, 480);
  EXPECT_EQ(fit.x, 60);
  EXPECT_EQ(fit.y, 160);
}

TEST(PagePictureFit, LandscapePictureKeepsReadingOrientation) {
  // No auto-rotation: 1600x900 on a portrait screen stays upright.
  const auto fit = page_picture::fitToScreen(1600, 900, 480, 800);
  EXPECT_FALSE(fit.rotate);
  EXPECT_EQ(fit.width, 472);
  EXPECT_EQ(fit.height, 266);
  EXPECT_EQ(fit.x, 4);
  EXPECT_EQ(fit.y, 267);
}

TEST(PagePictureFit, RequestedTurnLaysOutInTheTurnedFrame) {
  // The direction buttons ask for the quarter turn: frame 800x480.
  const auto fit = page_picture::fitToScreen(1600, 900, 480, 800, true);
  EXPECT_TRUE(fit.rotate);
  EXPECT_EQ(fit.width, 792);
  EXPECT_EQ(fit.height, 446);
  EXPECT_EQ(fit.x, 4);
  EXPECT_EQ(fit.y, 17);
}

TEST(PagePictureFit, PortraitPictureStaysUprightOnLandscapeScreen) {
  const auto fit = page_picture::fitToScreen(900, 1600, 800, 480);
  EXPECT_FALSE(fit.rotate);
  EXPECT_EQ(fit.width, 266);
  EXPECT_EQ(fit.height, 472);
  EXPECT_EQ(fit.x, 267);
  EXPECT_EQ(fit.y, 4);
}

TEST(PagePictureFit, LayoutAlwaysStaysInsideTheMargin) {
  for (const bool rotate : {false, true}) {
    for (int w = 1; w <= 4000; w += 37) {
      for (int h = 1; h <= 4000; h += 53) {
        const auto fit = page_picture::fitToScreen(w, h, 480, 800, rotate);
        const int frameW = rotate ? 800 : 480;
        const int frameH = rotate ? 480 : 800;
        const int m = page_picture::VIEW_MARGIN;
        ASSERT_EQ(fit.rotate, rotate);
        ASSERT_GE(fit.x, m);
        ASSERT_GE(fit.y, m);
        ASSERT_GE(fit.width, 1);
        ASSERT_GE(fit.height, 1);
        ASSERT_LE(fit.x + fit.width, frameW - m);
        ASSERT_LE(fit.y + fit.height, frameH - m);
        // Fit: one axis reaches the margin, or the 3x cap holds both.
        const bool filled = fit.width >= frameW - 2 * m - 1 || fit.height >= frameH - 2 * m - 1;
        const bool capped = fit.width <= 3 * w + 1 && fit.height <= 3 * h + 1 &&
                            (fit.width >= 3 * w - 1 || fit.height >= 3 * h - 1);
        ASSERT_TRUE(filled || capped) << w << "x" << h;
        ASSERT_LE(fit.width, 3 * w + 1);
      }
    }
  }
}

TEST(PagePictureFit, DegenerateInputGivesEmptyLayout) {
  const auto fit = page_picture::fitToScreen(0, 100, 480, 800);
  EXPECT_EQ(fit.width, 0);
  EXPECT_EQ(fit.height, 0);
}

TEST(PagePictureCloseGate, IgnoresTapsBeforeTheFrameAndDuringDebounce) {
  page_picture::ViewerCloseGate gate;
  // Nothing painted yet: the opening double tap's second tap lands here.
  EXPECT_FALSE(gate.closes(10, false, false));
  EXPECT_FALSE(gate.closes(20, false, true));
  gate.frameShown(1000);
  EXPECT_FALSE(gate.closes(1010, false, false));
  EXPECT_FALSE(gate.closes(1100, true, false));
  EXPECT_FALSE(gate.closes(1349, false, true));
}

TEST(PagePictureCloseGate, ContactHeldAcrossTheDebounceDoesNotClose) {
  page_picture::ViewerCloseGate gate;
  gate.frameShown(1000);
  EXPECT_FALSE(gate.closes(1200, true, false));
  EXPECT_FALSE(gate.closes(1400, true, false));  // finger still down
  EXPECT_FALSE(gate.closes(1450, false, true));  // its release
  EXPECT_FALSE(gate.closes(1460, false, false));  // lifted: now armed
  EXPECT_FALSE(gate.closes(1500, true, false));   // a fresh touch...
  EXPECT_TRUE(gate.closes(1600, false, true));    // ...and release closes
}

TEST(PagePictureCloseGate, FreshTapAfterDebounceCloses) {
  page_picture::ViewerCloseGate gate;
  gate.frameShown(1000);
  EXPECT_FALSE(gate.closes(1350, false, false));
  // A quick tap whose down and up both fall between two loop frames.
  EXPECT_TRUE(gate.closes(1400, false, true));
  // A new frame (a turn) re-arms the debounce.
  gate.frameShown(2000);
  EXPECT_FALSE(gate.closes(2100, false, false));
  EXPECT_FALSE(gate.closes(2200, false, true));
}

TEST(PagePictureFit, QuarterTurnPairsOrientations) {
  // Portrait <-> LandscapeClockwise, PortraitInverted <-> LandscapeCounterClockwise.
  EXPECT_EQ(page_picture::quarterTurnOrientation(0), 1);
  EXPECT_EQ(page_picture::quarterTurnOrientation(1), 0);
  EXPECT_EQ(page_picture::quarterTurnOrientation(2), 3);
  EXPECT_EQ(page_picture::quarterTurnOrientation(3), 2);
}

TEST(SmoothResample, ShrinkingAveragesTheCoveredBox) {
  // Half size: output 3 covers source [6, 8).
  const auto t = smooth_resample::taps(3, 2 * smooth_resample::FP_ONE, 0, 100);
  EXPECT_EQ(t.first, 6);
  EXPECT_EQ(t.last, 8);
  EXPECT_LT(t.weight, 0);
  const int samples[] = {0, 0, 0, 0, 0, 0, 100, 200, 0};
  EXPECT_EQ(smooth_resample::filter(t, [&](const int i) { return samples[i]; }), 150);
  // A box running past the block keeps at least its first sample.
  const auto edge = smooth_resample::taps(49, 2 * smooth_resample::FP_ONE, 0, 99);
  EXPECT_EQ(edge.first, 98);
  EXPECT_EQ(edge.last, 99);
}

TEST(SmoothResample, EnlargingBlendsAroundThePixelCentre) {
  // Double size: output 3's centre is source 1.25.
  const auto t = smooth_resample::taps(3, smooth_resample::FP_ONE / 2, 0, 10);
  EXPECT_EQ(t.first, 1);
  EXPECT_EQ(t.last, 2);
  EXPECT_EQ(t.weight, 64);
  const int samples[] = {0, 100, 200};
  EXPECT_EQ(smooth_resample::filter(t, [&](const int i) { return samples[i]; }), 125);
  // Edges clamp instead of reading outside the block.
  const auto first = smooth_resample::taps(0, smooth_resample::FP_ONE / 2, 0, 10);
  EXPECT_EQ(first.first, 0);
  const auto last = smooth_resample::taps(19, smooth_resample::FP_ONE / 2, 0, 10);
  EXPECT_EQ(last.first, 9);
  EXPECT_EQ(last.last, 9);
}

TEST(SmoothResample, DiffusionKeepsFlatFieldsAndTheirMeanTone) {
  constexpr int SIZE = 64;
  for (const int gray : {0, 40, 128, 200, 255}) {
    std::vector<int16_t> down(SIZE, 0);
    long total = 0;
    int levelsSeen = 0;
    for (int y = 0; y < SIZE; y++) {
      int carry = 0;
      for (int x = 0; x < SIZE; x++) {
        const uint8_t level = smooth_resample::diffuse(gray, down.data(), x, x == 0, carry);
        ASSERT_LE(level, 3);
        total += level * 85;
        levelsSeen |= 1 << level;
      }
    }
    const double mean = static_cast<double>(total) / (SIZE * SIZE);
    EXPECT_NEAR(mean, gray, 3.0) << gray;
    if (gray == 0) EXPECT_EQ(levelsSeen, 1);
    if (gray == 255) EXPECT_EQ(levelsSeen, 8);
    // A mid tone mixes the two nearest levels (1 = 85 and 2 = 170) only.
    if (gray == 128) EXPECT_EQ(levelsSeen, (1 << 1) | (1 << 2));
  }
}
