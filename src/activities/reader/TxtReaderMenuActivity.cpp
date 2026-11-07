#include "TxtReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <I18n.h>
#include <Memory.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingStatsActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

TxtReaderMenuActivity::TxtReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::string title, const Status& status)
    : UiListActivity("TxtReaderMenu", renderer, mappedInput), title(std::move(title)), status(status) {
  const auto add = [this](const MenuAction action, const StrId label) {
    if (menuItemCount < MAX_MENU_ITEMS) menuItems[menuItemCount++] = {action, label};
  };
  add(MenuAction::CONTENTS, StrId::STR_SELECT_CHAPTER);
  add(MenuAction::READING_STATS, StrId::STR_READING_STATS);
  add(MenuAction::GO_TO_PAGE, StrId::STR_GO_TO_PAGE);
  add(MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT);
  add(MenuAction::DICTIONARY, StrId::STR_DICTIONARY);
  if (Frontlight.present()) {
    add(MenuAction::FRONTLIGHT, StrId::STR_FRONTLIGHT);
  }
  add(MenuAction::REFRESH_SCREEN, StrId::STR_FORCE_REFRESH);

  for (size_t i = 0; i < menuItemCount; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuItems[i].labelId);
    item.actionValue = static_cast<int16_t>(i);
    menuRowItems[i] = item;
  }
}

void TxtReaderMenuActivity::closeCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  result.data = MenuResult{-1};
  setResult(std::move(result));
  finish();
}

bool TxtReaderMenuActivity::handleHomeGesture() {
  closeCancelled();
  return true;
}

void TxtReaderMenuActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(menuItemCount)) return;
  nav.selected = index;

  const auto selectedAction = menuItems[index].action;
  if (selectedAction == MenuAction::READING_STATS) {
    if (auto activity = makeUniqueNoThrow<ReadingStatsActivity>(renderer, mappedInput, true)) {
      startActivityForResult(std::move(activity), nullptr);
    } else
      LOG_ERR("STATS", "OOM: reading statistics");
    return;
  }
  if (selectedAction == MenuAction::FRONTLIGHT) {
    const bool lightOn = !Frontlight.isOn();
    Frontlight.setOn(lightOn);
    SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  // The activated row leaves this screen; a lingering flash would gray an unrelated element on the next render.
  app.clearTapFlash();
  setResult(MenuResult{static_cast<int>(selectedAction)});
  finish();
}

bool TxtReaderMenuActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    closeCancelled();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }

  return false;
}

void TxtReaderMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band GUI.drawHeader paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  // Position line; while the book is still being indexed the page count is
  // marked as an estimate and the indexing progress follows.
  char progressLine[160];
  int used = snprintf(progressLine, sizeof(progressLine), "%d/%s%d%s%s%d%%", status.page, status.indexing ? "~" : "",
                      status.pageCount, tr(STR_PAGES_SEPARATOR), tr(STR_BOOK_PREFIX), status.bookPercent);
  if (status.indexing && used > 0 && static_cast<size_t>(used) < sizeof(progressLine)) {
    snprintf(progressLine + used, sizeof(progressLine) - used, "  |  %s %d%%", tr(STR_INDEXING), status.indexPercent);
  }
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(metrics.tabBarHeight));
  const int16_t pad = screen.theme().headerSidePadding;
  fui::TextStyle progressStyle = screen.theme().smallText;
  progressStyle.align = UITheme::isRtl() ? fui::TextAlign::Right : fui::TextAlign::Left;
  screen.target().text(band.inset(fui::Insets{0, pad, 0, pad}), progressLine, progressStyle);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (size_t i = 0; i < menuItemCount; i++) {
    if (menuItems[i].action == MenuAction::FRONTLIGHT) {
      menuRowItems[i].value = I18N.get(Frontlight.isOn() ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
    }
  }

  fui::ListProps props;
  props.items = menuRowItems;
  props.count = static_cast<uint16_t>(menuItemCount);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.valueText = screen.theme().smallText;
  syncListViewport(screen, props);
  screen.list(props);
}

void TxtReaderMenuActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 title.c_str());
}
