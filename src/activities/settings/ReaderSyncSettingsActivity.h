#pragma once
#include "activities/UiListActivity.h"

class ReaderSyncSettingsActivity final : public UiListActivity {
 public:
  ReaderSyncSettingsActivity(GfxRenderer& renderer, MappedInputManager& input);
  void onEnter() override;
  void loop() override;
 private:
  static constexpr int ITEM_COUNT = 8;
  int listCount() const override { return ITEM_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  freeink::ui::ListItem rows[ITEM_COUNT]{};
  char lastSuccess[24]{};
  char books[24]{};
  char notes[48]{};
  char waitText[96]{};
  uint32_t revision = 0;
};
