#pragma once

#include <I18n.h>

#include "activities/UiListActivity.h"

class XtcReaderMenuActivity final : public UiListActivity {
 public:
  XtcReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("XtcReaderMenu", renderer, input) {}

 private:
  freeink::ui::ListItem rows[2]{};
  int listCount() const override { return 2; }
  const char* headerTitle() const override { return tr(STR_STATS_READER_MENU); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
};
