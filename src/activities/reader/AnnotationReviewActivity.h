#pragma once

#include <array>
#include <memory>

#include "AnnotationEntry.h"
#include "StudyCardStore.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Book-local review; Open source returns ProgressChangeResult to the owning reader.
class AnnotationReviewActivity final : public UiListActivity {
 public:
  AnnotationReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                           std::unique_ptr<AnnotationEntry> entry);
  AnnotationReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                           std::unique_ptr<StudyCardEntry> card);
  void onEnter() override;
  void onExit() override;
  void render(RenderLock&& lock) override;

 private:
  static constexpr size_t ROW_BYTES = 64;
  static constexpr size_t MAX_ROWS = 80;
  struct TextRow {
    const char* text = nullptr;
    const char* heading = nullptr;
    uint8_t length = 0;
  };
  std::unique_ptr<AnnotationEntry> annotation;
  std::unique_ptr<StudyCardEntry> card;
  std::array<TextRow, MAX_ROWS> rows{};
  size_t rowCount = 0;
  bool revealed = false;
  bool loaded = true;
  OptionPopup popup;
  char rowText[ROW_BYTES + 1] = {};
  char savedAt[32] = {};

  const AnnotationEntry* source() const;
  void appendText(const char* heading, const char* text);
  void rebuildRows();
  void openSource();
  void createCard();
  void editAnnotation();
  void deleteCard();
  void showMessage(StrId message);
  int listCount() const override;
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  static void provideRow(void* context, uint16_t index, freeink::ui::ListItem& item);
};
