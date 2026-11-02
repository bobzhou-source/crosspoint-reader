#include "XtcReaderMenuActivity.h"

#include <Memory.h>

#include "ReadingStatsActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;
void XtcReaderMenuActivity::activateIndex(int index) {
  if (index == 0) {
    setResult(MenuResult{0});
    finish();
  } else if (auto activity = makeUniqueNoThrow<ReadingStatsActivity>(renderer, mappedInput, true)) {
    startActivityForResult(std::move(activity), nullptr);
  } else
    LOG_ERR("STATS", "OOM: reading statistics");
}
void XtcReaderMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  rows[0].label = tr(STR_SELECT_CHAPTER);
  rows[1].label = tr(STR_READING_STATS);
  rows[1].actionValue = 1;
  fui::ListProps props;
  props.items = rows;
  props.count = 2;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}
