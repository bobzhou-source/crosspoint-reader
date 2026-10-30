#pragma once

#include <memory>
#include <string>

#include "AnnotationEntry.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class AnnotationListActivity final : public UiListActivity {
 public:
  AnnotationListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                         std::string title);
  void onEnter() override;
  void onExit() override;
  void render(RenderLock&& lock) override;

 private:
  std::string bookPath;
  std::string title;
  std::unique_ptr<AnnotationSummary[]> entries;
  size_t count = 0;
  bool loaded = false;
  OptionPopup popup;
  char rowSubtitle[80] = {};

  int listCount() const override;
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  static void provideRow(void* context, uint16_t index, freeink::ui::ListItem& item);
  void reload();
  void exportAnnotations();
  void openStudyCards();
  void showMessage(StrId message, const char* detail = nullptr);
};
