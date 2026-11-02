#pragma once

#include <array>
#include <memory>

#include "AnnotationEntry.h"
#include "StudyCardStore.h"
#include "ReaderTextSlices.h"
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
  static constexpr size_t MAX_FIELDS = 10;
  struct TextField {
    const char* text = nullptr;
    const char* heading = nullptr;
    uint16_t rowCount = 0;
    ReaderTextSlices::Cursor cursor;
  };
  std::unique_ptr<AnnotationEntry> annotation;
  std::unique_ptr<StudyCardEntry> card;
  std::array<TextField, MAX_FIELDS> fields{};
  size_t fieldCount = 0;
  bool rowsDirty = true;
  int16_t textWidth = 0;
  freeink::ui::TextStyle textStyle{};
  const freeink::ui::DrawTarget* textTarget = nullptr;
  size_t rowCount = 0;
  bool revealed = false;
  bool loaded = true;
  OptionPopup popup;
  char rowText[AnnotationEntry::MAX_EXCERPT_BYTES + 1] = {};
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
