#include "TxtReaderChapterSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <TxtChapters.h>
#include <TxtLineBreak.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UIScale.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

TxtReaderChapterSelectionActivity::TxtReaderChapterSelectionActivity(GfxRenderer& renderer,
                                                                     MappedInputManager& mappedInput,
                                                                     std::string sourcePath,
                                                                     const TxtPagination::PageIndexBuilder& pageIndex,
                                                                     const size_t currentOffset,
                                                                     const int indexPercent)
    : UiListActivity("TxtReaderChapterSelection", renderer, mappedInput),
      sourcePath(std::move(sourcePath)),
      pageIndex(pageIndex),
      currentOffset(currentOffset),
      indexPercent(indexPercent) {}

int TxtReaderChapterSelectionActivity::listCount() const {
  return static_cast<int>(std::min<size_t>(pageIndex.chapters().size(), UINT16_MAX));
}

void TxtReaderChapterSelectionActivity::onEnter() {
  UiListActivity::onEnter();

  // Same trade as the EPUB contents: let the list keep its row glyphs resident.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
  if (!Storage.openFileForRead("TCS", sourcePath, source)) {
    LOG_ERR("TCS", "Failed to open %s", sourcePath.c_str());
  }

  // Open on the chapter being read: the last heading at or before the reading position.
  const auto& chapters = pageIndex.chapters();
  const auto after = std::upper_bound(chapters.begin(), chapters.end(), currentOffset);
  currentChapter = after == chapters.begin() ? -1 : static_cast<int>(after - chapters.begin()) - 1;
  if (currentChapter >= listCount()) currentChapter = -1;
  nav.selected = std::max(currentChapter, 0);
}

void TxtReaderChapterSelectionActivity::onExit() {
  source.close();
  UiListActivity::onExit();
}

bool TxtReaderChapterSelectionActivity::readTitle(const size_t offset, std::string& title) {
  title.clear();
  if (!source || !source.seek(offset)) return false;
  char line[TITLE_BYTES];
  const int got = source.read(line, sizeof(line));
  if (got <= 0) return false;
  size_t length = 0;
  while (length < static_cast<size_t>(got) && line[length] != '\n') length++;
  length = TxtLineBreak::completePrefix(line, length);
  size_t trimmedLength = 0;
  const char* trimmed = TxtChapters::trimHeading(line, length, trimmedLength);
  title.assign(trimmed, trimmedLength);
  return true;
}

// Materializes the rows of the window starting at `start` (clamped): each title
// is a short read from the book, so this runs only when the viewport leaves the
// current window, then batch-prewarms the window's glyphs.
void TxtReaderChapterSelectionActivity::refreshRowWindow(const int start) {
  const int total = listCount();
  int clamped = start;
  if (clamped > total - ROW_WINDOW) clamped = total - ROW_WINDOW;
  if (clamped < 0) clamped = 0;
  if (clamped == windowStart) return;

  const auto& chapters = pageIndex.chapters();
  windowCount = total - clamped < ROW_WINDOW ? total - clamped : ROW_WINDOW;
  for (int i = 0; i < windowCount; i++) {
    const int row = clamped + i;
    const size_t offset = chapters[row];
    if (!readTitle(offset, windowLabels[i]) || windowLabels[i].empty()) {
      windowLabels[i] = tr(STR_UNNAMED);
    }
    fui::ListItem item;
    item.label = windowLabels[i].c_str();
    if (row == currentChapter) {
      item.value = tr(STR_CURRENT_CHAPTER);
    } else {
      snprintf(windowValues[i], sizeof(windowValues[i]), "%u",
               static_cast<unsigned>(pageIndex.pageContaining(offset) + 1));
      item.value = windowValues[i];
    }
    item.actionValue = static_cast<int16_t>(row);
    windowItems[i] = item;
  }
  windowStart = clamped;

  struct PrewarmCtx {
    const std::string* labels;
    int count;
  } prewarmCtx{windowLabels, windowCount};
  renderer.prewarmFallbackText(
      uiScaleSpec().bodyFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        const auto* c = static_cast<const PrewarmCtx*>(ctx);
        return i < static_cast<uint32_t>(c->count) ? c->labels[i].c_str() : nullptr;
      },
      &prewarmCtx, static_cast<uint32_t>(windowCount));
}

void TxtReaderChapterSelectionActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) {
    return;
  }
  // The activated row leaves this screen (finish); a lingering flash would gray
  // an unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;
  setResult(PageResult{static_cast<uint32_t>(pageIndex.pageContaining(pageIndex.chapters()[index]))});
  finish();
}

bool TxtReaderChapterSelectionActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }

  return false;
}

void TxtReaderChapterSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band drawChrome paints the title in.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  // While indexing, the list holds only the chapters found so far.
  if (!pageIndex.complete()) {
    char indexingLine[64];
    snprintf(indexingLine, sizeof(indexingLine), "%s %d%%", tr(STR_INDEXING), indexPercent);
    const fui::Rect band = screen.takeTop(static_cast<int16_t>(metrics.tabBarHeight));
    const int16_t pad = screen.theme().headerSidePadding;
    fui::TextStyle style = screen.theme().smallText;
    style.align = UITheme::isRtl() ? fui::TextAlign::Right : fui::TextAlign::Left;
    screen.target().text(band.inset(fui::Insets{0, pad, 0, pad}), indexingLine, style);
  }
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (listCount() == 0) {
    auto style = screen.theme().bodyText;
    style.align = fui::TextAlign::Center;
    style.maxLines = 4;
    screen.target().text(screen.body().inset(fui::Insets{0, 20, 0, 20}), tr(STR_NO_CHAPTERS), style);
    return;
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  props.valueText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  // Materialize the row window for the final viewport and hand list() the
  // window with its absolute base index.
  refreshRowWindow(nav.top);
  props.items = windowItems;
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  screen.list(props);
}

void TxtReaderChapterSelectionActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_SELECT_CHAPTER));
}
