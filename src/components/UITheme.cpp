#include "UITheme.h"

#include <FsHelpers.h>
#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/CoverGridHomeUi.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

UITheme UITheme::instance;

UITheme::UITheme() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::reload() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

bool UITheme::supportsCoverGrid() { return HalMemory::getPsramHeap().totalBytes > 0; }

bool UITheme::hasCoverGridHome() { return SETTINGS.uiTheme == CrossPointSettings::COVER_GRID && supportsCoverGrid(); }

void UITheme::drawCoverGridHome(CoverGridHomeUi& home) { home.renderUi(); }

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  if (type == CrossPointSettings::COVER_GRID && !supportsCoverGrid()) type = CrossPointSettings::LYRA;

  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::COVER_GRID:
    case CrossPointSettings::UI_THEME::LYRA: {
      // The cover home owns its screen-lifetime UI state; other screens retain Lyra styling.
      auto theme = makeUniqueNoThrow<LyraTheme>();
      if (!theme) {
        LOG_ERR("UI", "OOM: Lyra theme");
        return;
      }
      currentTheme = std::move(theme);
      currentMetrics = &LyraMetrics::values;
      LOG_DBG("UI", "Using Lyra theme");
      break;
    }
    case CrossPointSettings::UI_THEME::ROUNDEDRAFF:
      LOG_DBG("UI", "Using RoundedRaff theme");
      currentTheme = std::make_unique<RoundedRaffTheme>();
      currentMetrics = &RoundedRaffMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = std::make_unique<Lyra3CoversTheme>();
      currentMetrics = &Lyra3CoversMetrics::values;
      break;
  }
  metricsValid = false;
}

const ThemeMetrics& UITheme::getMetrics() const {
  // hasTouch() can flip once touch init completes after static construction, so the
  // cached copy is refreshed when the flag differs instead of copying the struct per call.
  const bool touch = gpio.hasTouch();
  if (!metricsValid || touch != metricsForTouch) {
    adjustedMetrics = *currentMetrics;
    if (touch) {
      adjustedMetrics.buttonHintsHeight = 0;
    }
    metricsForTouch = touch;
    metricsValid = true;
  }
  return adjustedMetrics;
}

// Screen area excluding the button hints
Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  auto orientation = renderer.getOrientation();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  Rect safeArea = Rect{0, 0, screenWidth, screenHeight};
  const ThemeMetrics& metrics = getMetrics();
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints) {
        safeArea.height -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints) {
        safeArea.x += metrics.buttonHintsHeight;
        safeArea.width -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints) {
        safeArea.y += metrics.buttonHintsHeight;
        safeArea.height -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints) {
        safeArea.width -= metrics.buttonHintsHeight;
      }
      break;
  }
  return safeArea;
}

Rect UITheme::getContentBounds(const GfxRenderer& renderer, const bool hasHeader) {
  const auto& metrics = getInstance().getMetrics();
  Rect bounds = getInstance().getScreenSafeArea(renderer, true);
  int top = 0, right = 0, bottom = 0, left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int x = std::max(bounds.x, left) + metrics.contentSidePadding;
  const int y = std::max(bounds.y, top) +
                (hasHeader ? metrics.topPadding + metrics.headerHeight : 0) + 16;
  const int endX = std::min(bounds.x + bounds.width, renderer.getScreenWidth() - right) - metrics.contentSidePadding;
  const int endY = std::min(bounds.y + bounds.height, renderer.getScreenHeight() - bottom) - 16;
  return Rect{x, y, std::max(0, endX - x), std::max(0, endY - y)};
}

bool UITheme::isRtl() {
  const auto language = I18N.getLanguage();
  return language == Language::AR || language == Language::HE || language == Language::FA;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  const auto sb = SETTINGS.statusBarSpec();

  // Layout reservation is hardware-agnostic: pass clockAvailable=true so the
  // reserved height does not depend on whether an RTC is present.
  return (sb.textLaneVisible(true) ? (metrics.statusBarVerticalMargin) : 0) +
         (sb.showsProgressBar() ? (sb.progressBarHeightPx + metrics.progressBarMarginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  const auto sb = SETTINGS.statusBarSpec();
  return sb.showsProgressBar() ? (sb.progressBarHeightPx + metrics.progressBarMarginTop) : 0;
}

// Centered text implementation that takes the safe area into account
void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, text, style)) / 2;
  renderer.drawText(fontId, x, y, text, black, style);
}

void UITheme::drawCenteredWrappedText(const GfxRenderer& renderer, Rect bounds, int fontId, const char* text,
                                      int maxLines, bool black, EpdFontFamily::Style style,
                                      TextVerticalAlignment verticalAlignment) {
  if (!text || *text == '\0' || bounds.width <= 0 || bounds.height <= 0 || maxLines <= 0) return;

  const int lineHeight = renderer.getLineHeight(fontId);
  if (lineHeight <= 0) return;

  const int lineLimit = std::min(maxLines, bounds.height / lineHeight);
  if (lineLimit <= 0) return;

  const auto alignedTop = [&](const int textHeight) {
    switch (verticalAlignment) {
      case TextVerticalAlignment::CENTER:
        return bounds.y + (bounds.height - textHeight) / 2;
      case TextVerticalAlignment::BOTTOM:
        return bounds.y + bounds.height - textHeight;
      case TextVerticalAlignment::TOP:
      default:
        return bounds.y;
    }
  };

  if (strchr(text, '\n') == nullptr && renderer.getTextWidth(fontId, text, style) <= bounds.width) {
    drawCenteredText(renderer, bounds, fontId, alignedTop(lineHeight), text, black, style);
    return;
  }

  const auto clip = renderer.getClipRect();
  const int left = std::max(bounds.x, clip[0]);
  const int top = std::max(bounds.y, clip[1]);
  renderer.setClipRect(left, top, std::max(0, std::min(bounds.x + bounds.width, clip[0] + clip[2]) - left),
                       std::max(0, std::min(bounds.y + bounds.height, clip[1] + clip[3]) - top));
  if (style == EpdFontFamily::REGULAR || style == EpdFontFamily::BOLD) {
    // FUI emits bounded UTF-8 runs without allocating a vector of line strings.
    namespace fui = freeink::ui;
    fui::GfxRendererTarget target(renderer);
    target.setFont(fui::GfxRendererTarget::FONT_BODY, fontId);
    fui::TextStyle textStyle;
    textStyle.font = fui::GfxRendererTarget::FONT_BODY;
    textStyle.bold = style == EpdFontFamily::BOLD;
    textStyle.align = fui::TextAlign::Center;
    textStyle.maxLines = static_cast<uint8_t>(std::min(lineLimit, 16));
    const int textHeight = fui::measureWrappedText(target, text, textStyle, bounds.width).height;
    const fui::Rect textBounds{static_cast<int16_t>(bounds.x), static_cast<int16_t>(alignedTop(textHeight)),
                               static_cast<int16_t>(bounds.width), static_cast<int16_t>(textHeight)};
    fui::layoutText(target, textBounds, text, textStyle, [&](const char* line, const fui::Rect run) {
      renderer.drawText(fontId, run.x, run.y, line, black, style);
    });
  } else {
    // The legacy helper also supports italic/superscript styles not exposed by the UI target.
    const auto lines = renderer.wrappedText(fontId, text, bounds.width, lineLimit, style);
    int y = alignedTop(static_cast<int>(lines.size()) * lineHeight);
    for (const auto& line : lines) {
      drawCenteredText(renderer, bounds, fontId, y, line.c_str(), black, style);
      y += lineHeight;
    }
  }
  renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);
}
