#pragma once

#include <memory>

#include "AnnotationStore.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class ReadingInboxActivity final : public UiListActivity {
 public:
  ReadingInboxActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void render(RenderLock&& lock) override;

 private:
  static constexpr size_t PAGE_SIZE = 32;
  // Display-only previews; the source key + native ID reload the full record.
  struct Row {
    char sourceKey[17] = {};
    uint16_t id = 0;
    AnnotationKind kind = AnnotationKind::Quote;
    uint8_t percentage = 0;
    char title[97] = {};
    char sourceChapter[193] = {};
  };
  std::unique_ptr<Row[]> rows;
  size_t pageStart = 0;
  size_t count = 0;
  size_t total = 0;
  bool loaded = false;
  OptionPopup popup;
  char rowValue[80] = {};
  char pageLabel[80] = {};

  int listCount() const override;
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  bool handleCustomInput() override;
  static void provideRow(void* context, uint16_t index, freeink::ui::ListItem& item);
  static AnnotationStore::Result collectEntry(const AnnotationEntry& entry, void* context);
  void reload();
  void openEntry(size_t index);
  void exportInbox();
  void showMessage(StrId message, const char* detail = nullptr);
};
