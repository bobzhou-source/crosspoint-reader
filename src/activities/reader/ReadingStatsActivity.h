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
  freeink::ui::ListItem rows[14]{};
  char labels[14][96]{};
  char values[14][72]{};
  int listCount() const override { return count; }
  const char* headerTitle() const override { return tr(STR_READING_STATS); }
  void activateIndex(int index) override;
  void buildScreen(UiScreen& screen) override;
  void add(const char* label, const char* value);
};
