#include <Epub/Epub/PagePicture.h>
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

TEST(PagePictureHitTest, TapsBesideThePictureKeepThePage) {
  const std::vector<PagePicture> pictures{picture(20, 100, 200, 150)};
  // One pixel outside each edge: no finger slop, these stay page-turn taps.
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 35, 200, 16, 40), -1);
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 236, 200, 16, 40), -1);
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 130, 139, 16, 40), -1);
  EXPECT_EQ(page_picture::pictureAtPoint(pictures, 130, 290, 16, 40), -1);
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

TEST(PagePictureFit, PortraitPictureFillsPortraitScreen) {
  const auto fit = page_picture::fitToScreen(1000, 1500, 480, 800);
  EXPECT_FALSE(fit.rotate);
  EXPECT_EQ(fit.width, 480);
  EXPECT_EQ(fit.height, 720);
  EXPECT_EQ(fit.x, 0);
  EXPECT_EQ(fit.y, 40);
}

TEST(PagePictureFit, SmallPictureIsScaledUpToTheScreen) {
  const auto fit = page_picture::fitToScreen(120, 160, 480, 800);
  EXPECT_FALSE(fit.rotate);
  EXPECT_EQ(fit.width, 480);
  EXPECT_EQ(fit.height, 640);
  EXPECT_EQ(fit.y, 80);
}

TEST(PagePictureFit, LandscapePictureTurnsOnPortraitScreen) {
  // 1600x900 upright would be 480x270; a quarter turn gives 800x450.
  const auto fit = page_picture::fitToScreen(1600, 900, 480, 800);
  EXPECT_TRUE(fit.rotate);
  EXPECT_EQ(fit.width, 800);
  EXPECT_EQ(fit.height, 450);
  EXPECT_EQ(fit.x, 0);
  EXPECT_EQ(fit.y, 15);
}

TEST(PagePictureFit, NearSquarePictureStaysUpright) {
  // 500x460: turned would gain under 15%, not worth rotating the device for.
  const auto fit = page_picture::fitToScreen(500, 460, 480, 800);
  EXPECT_FALSE(fit.rotate);
  EXPECT_EQ(fit.width, 480);
  EXPECT_EQ(fit.height, 442);
}

TEST(PagePictureFit, PortraitPictureTurnsOnLandscapeScreen) {
  const auto fit = page_picture::fitToScreen(900, 1600, 800, 480);
  EXPECT_TRUE(fit.rotate);
  EXPECT_EQ(fit.width, 450);
  EXPECT_EQ(fit.height, 800);
}

TEST(PagePictureFit, LayoutAlwaysStaysOnScreen) {
  for (int w = 1; w <= 4000; w += 37) {
    for (int h = 1; h <= 4000; h += 53) {
      const auto fit = page_picture::fitToScreen(w, h, 480, 800);
      const int frameW = fit.rotate ? 800 : 480;
      const int frameH = fit.rotate ? 480 : 800;
      ASSERT_GE(fit.x, 0);
      ASSERT_GE(fit.y, 0);
      ASSERT_GE(fit.width, 1);
      ASSERT_GE(fit.height, 1);
      ASSERT_LE(fit.x + fit.width, frameW);
      ASSERT_LE(fit.y + fit.height, frameH);
      // Fit means one axis reaches the screen edge (within rounding).
      ASSERT_TRUE(fit.width >= frameW - 1 || fit.height >= frameH - 1) << w << "x" << h;
    }
  }
}

TEST(PagePictureFit, DegenerateInputGivesEmptyLayout) {
  const auto fit = page_picture::fitToScreen(0, 100, 480, 800);
  EXPECT_EQ(fit.width, 0);
  EXPECT_EQ(fit.height, 0);
}

TEST(PagePictureFit, QuarterTurnPairsOrientations) {
  // Portrait <-> LandscapeClockwise, PortraitInverted <-> LandscapeCounterClockwise.
  EXPECT_EQ(page_picture::quarterTurnOrientation(0), 1);
  EXPECT_EQ(page_picture::quarterTurnOrientation(1), 0);
  EXPECT_EQ(page_picture::quarterTurnOrientation(2), 3);
  EXPECT_EQ(page_picture::quarterTurnOrientation(3), 2);
}
