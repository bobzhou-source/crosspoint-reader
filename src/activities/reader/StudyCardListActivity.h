#pragma once

#include <memory>
#include <string>

#include "StudyCardStore.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class StudyCardListActivity final : public UiListActivity {
 public:
  StudyCardListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath);
  void onEnter() override;
  void onExit() override;
  void render(RenderLock&& lock) override;

 private:
  std::string bookPath;
  std::unique_ptr<StudyCardSummary[]> entries;
  size_t count = 0;
  bool loaded = false;
  OptionPopup popup;
  char rowSubtitle[80] = {};

  void reload();
  void showLoadError();
  int listCount() const override;
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  static void provideRow(void* context, uint16_t index, freeink::ui::ListItem& item);
};
