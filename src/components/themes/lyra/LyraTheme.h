#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Lyra theme metrics (zero runtime cost)
namespace LyraMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 16,
                                 .batteryHeight = 12,
                                 // Shared anchor for every header band, including the cover-grid
                                 // home's status band; roomy enough that the strip clears the edge.
                                 .topPadding = 10,
                                 .batteryBarHeight = 24,
                                 .headerHeight = 84,
                                 .verticalSpacing = 16,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 56,
                                 .listWithSubtitleRowHeight = 76,
                                 .listRowGap = 0,
                                 .listRowRadius = 0,
                                 .listInset = 12,
                                 .listSidePadding = 8,
                                 .listSelectionStyle = 2,  // underline
                                 .listScrollWidth = 2,
                                 .listScrollSide = 0,
                                 .listTitleBold = false,
                                 .headerSidePadding = 20,
                                 .headerUnderlineSize = 1,
                                 .headerTitleAlign = 0,  // left
                                 .headerBatterySide = 0,
                                 .headerClockCentered = false,
                                 .menuRowHeight = 56,
                                 .menuSpacing = 0,
                                 .tabSpacing = 8,
                                 .tabBarHeight = 48,
                                 .scrollBarWidth = 2,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 110,
                                 .homeCoverHeight = 180,
                                 .homeCoverTileHeight = 212,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 16,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyHeight = 48,
                                 .keyboardKeySpacing = 0,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -7,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 94,
                                 .popupTopOffsetRatio = 0.165f,
                                 .popupMarginX = 16,
                                 .popupMarginY = 12,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 0,
                                 .popupTextBold = false,
                                 .popupTextInverted = true,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = true,
                                 .popupProgressFillInverted = true,
                                 .popupProgressOutlineInverted = true,
                                 .optionPopupItemSpacing = 8,
                                 .optionPopupInnerPadding = 20,
                                 .optionPopupSelectionVPadding = 12,
                                 .optionPopupDialogSideMargin = 20,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0,
                                 .controlRadius = 0,
                                 .sheetRadius = 0,
                                 .capsuleRadius = 0};
}  // namespace LyraMetrics

class LyraTheme : public BaseTheme {
 public:
  // Component drawing methods
  void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const override;
  int getMenuRowHeight(const GfxRenderer& renderer) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  void drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const;
  bool showsFileIcons() const override { return true; }
  int homeCoverThumbHeight(const GfxRenderer&, int recentBookCount) const override;
};
