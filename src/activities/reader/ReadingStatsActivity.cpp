#include "ReadingStatsActivity.h"

#include <Memory.h>

#include <cstdio>

#include "ReadingStats.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;
namespace {
void duration(uint64_t milliseconds, char* out, size_t size) {
  const auto seconds = milliseconds / 1000;
  snprintf(out, size, "%llu:%02u:%02u", static_cast<unsigned long long>(seconds / 3600),
           static_cast<unsigned>((seconds / 60) % 60), static_cast<unsigned>(seconds % 60));
}
void total(const reading_stats::Totals& totals, char* out, size_t size) {
  char time[32];
  duration(totals.milliseconds, time, sizeof(time));
  snprintf(out, size, tr(STR_STATS_TOTAL_FORMAT), time, static_cast<unsigned>(totals.pages));
}
}  // namespace
void ReadingStatsActivity::add(const char* label, const char* value) {
  if (count >= MAX_ROWS) return;
  snprintf(labels[count], sizeof(labels[count]), "%s", label);
  snprintf(values[count], sizeof(values[count]), "%s", value);
  rows[count].label = labels[count];
  rows[count].value = values[count];
  rows[count].actionValue = count;
  ++count;
}
void ReadingStatsActivity::onEnter() {
  count = 0;
  const auto& stats = reading_stats::data();
  char value[72];
  if (thisBook) {
    const auto& book = stats.book;
    add(book.title, tr(STR_STATS_THIS_BOOK));
    duration(book.totals.milliseconds, value, sizeof(value));
    add(tr(STR_STATS_ACTIVE_TIME), value);
    snprintf(value, sizeof(value), "%u", static_cast<unsigned>(book.totals.pages));
    add(tr(STR_STATS_PAGES), value);
    snprintf(value, sizeof(value), "%u", static_cast<unsigned>(book.totals.sessions));
    add(tr(STR_STATS_SESSIONS), value);
    if (book.totals.pages)
      duration(book.totals.milliseconds / book.totals.pages, value, sizeof(value));
    else
      snprintf(value, sizeof(value), "--");
    add(tr(STR_STATS_PACE), value);
    if (book.advanced && book.totals.milliseconds >= 60000) {
      duration(book.totals.milliseconds * (10000 - std::min<uint32_t>(book.position, 10000U)) / book.advanced,
               value, sizeof(value));
    } else
      snprintf(value, sizeof(value), "--");
    add(tr(STR_STATS_TIME_LEFT), value);
    snprintf(value, sizeof(value), "%.2f%%", book.advanced / 100.0);
    add(tr(STR_STATS_ADVANCED), value);
    reading_stats::formatDay(book.firstDay, value, sizeof(value));
    add(tr(STR_STATS_FIRST_READ), value);
    reading_stats::formatDay(book.lastDay, value, sizeof(value));
    add(tr(STR_STATS_LAST_READ), value);
    add(tr(STR_STATS_OVERVIEW), "");
  } else {
    const uint32_t day = reading_stats::today();
    if (day) {
      const reading_stats::Habit habit = stats.habit(day);
      snprintf(value, sizeof(value), tr(STR_STATS_DAYS), static_cast<unsigned>(habit.currentStreakDays));
      add(tr(STR_STATS_CURRENT_STREAK), value);
      snprintf(value, sizeof(value), tr(STR_STATS_DAYS), static_cast<unsigned>(habit.bestStreakDays));
      add(tr(STR_STATS_BEST_STREAK), value);
    } else {
      add(tr(STR_STATS_CURRENT_STREAK), tr(STR_STATS_SET_CLOCK));
    }
    total(stats.recent(day, 1), value, sizeof(value));
    add(tr(STR_STATS_TODAY), day ? value : tr(STR_STATS_SET_CLOCK));
    total(stats.recent(day, 7), value, sizeof(value));
    add(tr(STR_STATS_WEEK), day ? value : "--");
    total(stats.recent(day, 30), value, sizeof(value));
    add(tr(STR_STATS_MONTH), day ? value : "--");
    add(tr(STR_STATS_TOP_BOOKS), "");
    for (const auto& book : stats.summary.top) {
      if (!book.id) continue;
      duration(book.milliseconds, value, sizeof(value));
      add(book.title, value);
    }
  }
  UiListActivity::onEnter();
}
void ReadingStatsActivity::activateIndex(int index) {
  if (thisBook && index == count - 1) {
    if (auto activity = makeUniqueNoThrow<ReadingStatsActivity>(renderer, mappedInput)) {
      startActivityForResult(std::move(activity), nullptr);
    } else
      LOG_ERR("STATS", "OOM: statistics overview");
  }
}
void ReadingStatsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  fui::ListProps props;
  props.items = rows;
  props.count = count;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.valueText = screen.theme().smallText;
  syncListViewport(screen, props);
  screen.list(props);
}
