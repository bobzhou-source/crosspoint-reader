#include "LyraTheme.h"

#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/blocks.h"
#include "components/icons/book.h"
#include "components/icons/bookmark.h"
#include "components/icons/folder.h"
#include "components/icons/hotspot.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 0;
constexpr int topHintButtonY = 345;
constexpr int mainMenuIconSize = 32;

const uint8_t* iconForName(UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:
      return FolderIcon;
    case UIIcon::Book:
      return BookIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Library:
      return LibraryIcon;
    case UIIcon::Wifi:
      return WifiIcon;
    case UIIcon::Hotspot:
      return HotspotIcon;
    case UIIcon::Bookmark:
      return BookmarkIcon;
    case UIIcon::Blocks:
      return BlocksIcon;
    default:
      return nullptr;
  }
}
}  // namespace

void LyraTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();

  if (charging) {
    // Solid fill when charging so lightning bolt is visible
    renderer.fillRect(rect.x + 2, rect.y + 2, rect.width - 5, rect.height - 4);
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
  } else {
    if (percentage > 10) {
      renderer.fillRect(rect.x + 2, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 40) {
      renderer.fillRect(rect.x + 6, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 70) {
      renderer.fillRect(rect.x + 10, rect.y + 2, 3, rect.height - 4);
    }
  }
}

void LyraTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  const int contentWidth = std::max(0, rect.width - LyraMetrics::values.contentSidePadding * 2);

  int labelWidth = contentWidth;
  if (rightLabel) {
    auto truncatedRightLabel = renderer.truncatedText(SMALL_FONT_ID, rightLabel, contentWidth, EpdFontFamily::REGULAR);
    const int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - LyraMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    labelWidth = std::max(0, contentWidth - rightLabelWidth - hPaddingInSelection);
  }

  if (labelWidth > 0) {
    auto truncatedLabel = renderer.truncatedText(UI_10_FONT_ID, label, labelWidth, EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding, rect.y + 6,
                      truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  if (gpio.hasTouch()) {
    return;
  }

  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  constexpr int buttonHeight = LyraMetrics::values.buttonHintsHeight;
  constexpr int buttonY = LyraMetrics::values.buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  // Keyed to the portrait panel width: the 528-wide X3 gets more spacing than
  // the 480-wide boards (X4, X4 Pro, and the other 800x480 panels).
  constexpr int narrowButtonPositions[] = {58, 146, 254, 342};
  constexpr int wideButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = renderer.getScreenWidth() >= 528 ? wideButtonPositions : narrowButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const bool grayscale = renderer.getRenderMode() != GfxRenderer::BW && !renderer.grayPlanesAreAbsolute();

  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      // Draw the filled background and border for a FULL-sized button
      renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, cornerRadius,
                               grayscale ? Color::Black : Color::White);
      if (grayscale) continue;
      renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, cornerRadius, true, true, false,
                               false, true);
      drawHintLabel(renderer, SMALL_FONT_ID, labels[i], x, buttonWidth, pageHeight - buttonY, buttonHeight,
                    textYOffset);
    } else {
      // Draw the filled background and border for a SMALL-sized button
      renderer.fillRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, cornerRadius,
                               grayscale ? Color::Black : Color::White);
      if (grayscale) continue;
      renderer.drawRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, 1, cornerRadius, true,
                               true, false, false, true);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void LyraTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (gpio.hasTouch()) {
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = LyraMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 78;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 0;

  if (gpio.hasEdgeSideButtons()) {
    // Edge-button layout (X3, X4 Pro): Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(buttonMargin, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, false, true, false,
                               true, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, buttonMargin, x3ButtonY + (buttonHeight + textWidth) / 2, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonWidth;
      renderer.drawRoundedRect(rightX, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, rightX, x3ButtonY + (buttonHeight + textWidth) / 2, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, cornerRadius, true,
                               false, true, false, true);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topHintButtonY + (i * buttonHeight) + 5;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + (buttonHeight + textWidth) / 2, labels[i]);
      }
    }
  }
}

int LyraTheme::homeCoverThumbHeight(const GfxRenderer&, int) const {
  return LyraMetrics::values.homeCoverHeight;
}

void LyraTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }
  namespace fui = freeink::ui;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int count = std::min(static_cast<int>(recentBooks.size()), metrics.homeRecentBooksCount);
  const int width = std::max(0, rect.width - 2 * metrics.contentSidePadding);
  const int columnWidth = width / count;
  const int thumbHeight = homeCoverThumbHeight(renderer, count);
  const bool single = count == 1;
  const bool rtl = UITheme::isRtl();
  constexpr int actionHeight = 48;
  fui::GfxRendererTarget target(renderer);
  target.setFont(fui::GfxRendererTarget::FONT_SMALL, UI_10_FONT_ID);
  struct BookLayout {
    Rect cover;
    Rect title;
    Rect author;
    Rect action;
    int font;
    int titleLines;
  };
  const auto bookLayout = [&](int index) {
    BookLayout layout{};
    layout.font = titleFontId(renderer, recentBooks[index].title.c_str());
    const int titleLineHeight = renderer.getLineHeight(layout.font);
    const int metadataHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int authorHeight = !recentBooks[index].author.empty() &&
                                     rect.height >= titleLineHeight + metadataHeight + 8 + actionHeight + 16
                                 ? metadataHeight : 0;
    const int authorBlock = authorHeight > 0 ? authorHeight + 8 : 0;
    layout.titleLines = std::clamp((rect.height - authorBlock - actionHeight - 16) / titleLineHeight, 1, 2);
    if (!single && rect.height < thumbHeight + 8 + 2 * titleLineHeight + authorBlock + 12 + actionHeight) {
      layout.titleLines = 1;
    }
    int coverHeight = single ? std::min(thumbHeight, rect.height)
        : std::min(thumbHeight, std::max(0, rect.height - layout.titleLines * titleLineHeight -
                                             authorBlock - actionHeight - 20));
    if (coverHeight < 32) coverHeight = 0;
    const int coverWidth = std::max(0, std::min(coverHeight * 2 / 3,
                                               single ? columnWidth / 3 : columnWidth - 12));
    const int column = rtl ? count - 1 - index : index;
    const int x = rect.x + metrics.contentSidePadding + column * columnWidth;
    layout.cover = Rect{single ? (rtl ? x + columnWidth - coverWidth : x) : x + (columnWidth - coverWidth) / 2,
                        single ? rect.y + (rect.height - coverHeight) / 2 : rect.y, coverWidth, coverHeight};
    const int textX = single ? (rtl ? x : layout.cover.x + coverWidth + 20) : x + 6;
    const int textWidth = single ? columnWidth - coverWidth - 20 : columnWidth - 12;
    target.setFont(fui::GfxRendererTarget::FONT_BODY, layout.font);
    fui::TextStyle title;
    title.font = fui::GfxRendererTarget::FONT_BODY;
    title.maxLines = layout.titleLines;
    const int titleHeight = fui::measureWrappedText(target, recentBooks[index].title.c_str(), title, textWidth).height;
    const int blockHeight = titleHeight + authorBlock + 16 + actionHeight;
    const int textY = single ? rect.y + std::max(0, (rect.height - blockHeight) / 2)
                            : rect.y + coverHeight + (coverHeight > 0 ? 8 : 0);
    const int actionY = std::min(rect.y + rect.height - actionHeight,
                                 single ? textY + blockHeight - actionHeight : rect.y + rect.height - actionHeight);
    const int availableTitleHeight = std::max(0, actionY - textY - authorBlock - 8);
    layout.title = Rect{textX, textY, textWidth, std::min(titleHeight, availableTitleHeight)};
    layout.author = Rect{textX, textY + layout.title.height + 8, textWidth, authorHeight};
    layout.action = Rect{textX, actionY, textWidth, actionHeight};
    return layout;
  };

  if (!coverRendered || !bufferRestored) {
    for (int i = 0; i < count; ++i) {
      const Rect cover = bookLayout(i).cover;
      if (cover.width <= 0 || cover.height <= 0) continue;
      bool hasCover = false;
      if (!recentBooks[i].coverBmpPath.empty()) {
        const std::string path = UITheme::getCoverThumbPath(recentBooks[i].coverBmpPath, thumbHeight);
        HalFile file;
        if (Storage.openFileForRead("HOME", path, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            hasCover = drawCoverThumbFill(renderer, bitmap, cover);
          }
        }
      }
      if (!hasCover) {
        renderer.drawRect(cover.x, cover.y, cover.width, cover.height);
        const int spineX = rtl ? cover.x + cover.width - 8 : cover.x + 8;
        renderer.drawLine(spineX, cover.y + 1, spineX, cover.y + cover.height - 2);
        if (cover.width >= 48 && cover.height >= 48) {
          renderer.drawIcon(BookIcon, cover.x + (cover.width - 32) / 2, cover.y + (cover.height - 32) / 2, 32);
        }
      }
    }
    // Cache only static art; the selected action and text are drawn afterward.
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  for (int i = 0; i < count; ++i) {
    const auto& book = recentBooks[i];
    const BookLayout layout = bookLayout(i);
    fui::TextStyle title;
    title.font = fui::GfxRendererTarget::FONT_BODY;
    title.maxLines = layout.titleLines;
    title.align = rtl ? fui::TextAlign::Right : fui::TextAlign::Left;
    target.text({static_cast<int16_t>(layout.title.x), static_cast<int16_t>(layout.title.y),
                 static_cast<int16_t>(layout.title.width), static_cast<int16_t>(layout.title.height)},
                book.title.c_str(), title);
    fui::TextStyle secondary;
    secondary.font = fui::GfxRendererTarget::FONT_SMALL;
    secondary.align = title.align;
    if (layout.author.height > 0) {
      target.text({static_cast<int16_t>(layout.author.x), static_cast<int16_t>(layout.author.y),
                   static_cast<int16_t>(layout.author.width), static_cast<int16_t>(layout.author.height)},
                  book.author.c_str(), secondary);
    }
    const bool selected = selectorIndex == i;
    const Rect action = layout.action;
    if (selected) {
      renderer.fillRect(action.x, action.y, action.width, action.height);
    } else {
      renderer.drawLine(action.x, action.y + action.height - 1, action.x + action.width - 1,
                        action.y + action.height - 1);
    }
    secondary.align = fui::TextAlign::Center;
    secondary.color = selected ? fui::Color::White : fui::Color::Black;
    target.text({static_cast<int16_t>(action.x + 4), static_cast<int16_t>(action.y),
                 static_cast<int16_t>(action.width - 8), static_cast<int16_t>(action.height)},
                single ? tr(STR_CONTINUE_READING) : tr(STR_RESUME), secondary);
  }
}

void LyraTheme::drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const {
  namespace fui = freeink::ui;
  fui::GfxRendererTarget target(renderer);
  target.setFont(fui::GfxRendererTarget::FONT_BODY, titleFontId(renderer, tr(STR_NO_OPEN_BOOK)));
  target.setFont(fui::GfxRendererTarget::FONT_SMALL, UI_10_FONT_ID);
  fui::TextStyle title;
  title.font = fui::GfxRendererTarget::FONT_BODY;
  title.maxLines = std::clamp((rect.height - renderer.getLineHeight(UI_10_FONT_ID) - 8) /
                                  target.lineHeight(fui::GfxRendererTarget::FONT_BODY), 1, 2);
  title.align = UITheme::isRtl() ? fui::TextAlign::Right : fui::TextAlign::Left;
  fui::TextStyle message = title;
  message.font = fui::GfxRendererTarget::FONT_SMALL;
  message.maxLines = 2;
  const int padding = UITheme::getInstance().getMetrics().contentSidePadding;
  const int width = std::max(0, rect.width - 2 * padding);
  const int titleHeight = fui::measureWrappedText(target, tr(STR_NO_OPEN_BOOK), title, width).height;
  const int messageHeight = std::min(std::max(0, rect.height - titleHeight - 8),
                                    static_cast<int>(fui::measureWrappedText(target, tr(STR_START_READING), message,
                                                                           width).height));
  const int y = rect.y + std::max(0, (rect.height - titleHeight - messageHeight - 8) / 2);
  target.text({static_cast<int16_t>(rect.x + padding), static_cast<int16_t>(y), static_cast<int16_t>(width),
               static_cast<int16_t>(titleHeight)}, tr(STR_NO_OPEN_BOOK), title);
  target.text({static_cast<int16_t>(rect.x + padding), static_cast<int16_t>(y + titleHeight + 8),
               static_cast<int16_t>(width), static_cast<int16_t>(messageHeight)}, tr(STR_START_READING), message);
}

int LyraTheme::getMenuRowHeight(const GfxRenderer& renderer) const {
  return std::max(LyraMetrics::values.menuRowHeight, renderer.getLineHeight(UI_12_FONT_ID) + 16);
}

void LyraTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  namespace fui = freeink::ui;
  fui::GfxRendererTarget target(renderer);
  target.setFont(fui::GfxRendererTarget::FONT_BODY, UI_12_FONT_ID);
  fui::TextStyle text;
  text.font = fui::GfxRendererTarget::FONT_BODY;
  text.align = UITheme::isRtl() ? fui::TextAlign::Right : fui::TextAlign::Left;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowHeight = getMenuRowHeight(renderer);
  for (int i = 0; i < buttonCount; ++i) {
    const Rect row{rect.x + metrics.contentSidePadding, rect.y + i * (rowHeight + metrics.menuSpacing),
                   rect.width - metrics.contentSidePadding * 2, rowHeight};
    if (row.y + row.height > rect.y + rect.height) break;
    const bool rtl = UITheme::isRtl();
    if (selectedIndex == i) {
      renderer.fillRect(rtl ? row.x + row.width - 2 : row.x, row.y + 12, 2, row.height - 24);
      renderer.drawLine(row.x + 12, row.y + row.height - 1, row.x + row.width - 13, row.y + row.height - 1);
    }
    const uint8_t* icon = rowIcon ? iconForName(rowIcon(i)) : nullptr;
    const int iconWidth = icon ? mainMenuIconSize + 12 : 0;
    if (icon) {
      renderer.drawIcon(icon, rtl ? row.x + row.width - 12 - mainMenuIconSize : row.x + 12,
                        row.y + (rowHeight - mainMenuIconSize) / 2, mainMenuIconSize);
    }
    const std::string label = buttonLabel(i);
    target.text({static_cast<int16_t>(row.x + 12 + (rtl ? 0 : iconWidth)), static_cast<int16_t>(row.y),
                 static_cast<int16_t>(row.width - 24 - iconWidth), static_cast<int16_t>(row.height)},
                label.c_str(), text);
  }
}
