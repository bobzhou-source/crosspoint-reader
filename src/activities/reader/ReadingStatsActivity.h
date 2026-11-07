#pragma once

#include <I18n.h>

#include "activities/UiListActivity.h"

class ReadingStatsActivity final : public UiListActivity {
 public:
  ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& input, bool thisBook = false)
      : UiListActivity("ReadingStats", renderer, input), thisBook(thisBook) {}
  void onEnter() override;

 private:
  bool thisBook;
  int count = 0;
  // Overview: two streak rows, three windows, the top-books heading and every
  // top book must all fit, or the last book would be dropped silently.
  static constexpr int MAX_ROWS = 16;
  freeink::ui::ListItem rows[MAX_ROWS]{};
  char labels[MAX_ROWS][96]{};
  char values[MAX_ROWS][72]{};
  int listCount() const override { return count; }
  const char* headerTitle() const override { return tr(STR_READING_STATS); }
  void activateIndex(int index) override;
  void buildScreen(UiScreen& screen) override;
  void add(const char* label, const char* value);
};
