#include "HabitBadge.h"

#include <I18n.h>

#include <cstdio>

#include "ReadingStats.h"

const char* habitBadge(char* buffer, const size_t size) {
  if (!buffer || size < 2) return nullptr;
  const uint32_t day = reading_stats::today();
  if (!day) return nullptr;
  const reading_stats::Habit habit = reading_stats::data().habit(day);
  if (habit.currentStreakDays && habit.todayPagesRead) {
    snprintf(buffer, size, tr(STR_HABIT_PILL), static_cast<unsigned>(habit.currentStreakDays),
             static_cast<unsigned>(habit.todayPagesRead));
  } else if (habit.currentStreakDays) {
    snprintf(buffer, size, tr(STR_HABIT_PILL_STREAK_ONLY), static_cast<unsigned>(habit.currentStreakDays));
  } else if (habit.todayPagesRead) {
    snprintf(buffer, size, tr(STR_HABIT_PILL_PAGES_ONLY), static_cast<unsigned>(habit.todayPagesRead));
  } else {
    return nullptr;
  }
  return buffer;
}
