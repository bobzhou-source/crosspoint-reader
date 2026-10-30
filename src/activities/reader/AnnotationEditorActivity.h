#pragma once

#include <array>
#include <memory>

#include "AnnotationEntry.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Owns a draft until Save; cancelling the keyboard or backing out never writes.
// A nonzero entry.id edits an existing annotation. No new ActivityResult variant.
class AnnotationEditorActivity final : public UiListActivity {
 public:
  AnnotationEditorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                           std::unique_ptr<AnnotationEntry> entry);
  void onEnter() override;
  void onExit() override;
  void render(RenderLock&& lock) override;

 private:
  static constexpr size_t CONTEXT_ROW_BYTES = 96;
  static constexpr size_t MAX_CONTEXT_ROWS = 9;
  std::unique_ptr<AnnotationEntry> entry;
  std::array<std::array<char, CONTEXT_ROW_BYTES + 1>, MAX_CONTEXT_ROWS> contextRows{};
  size_t contextRowCount = 0;
  OptionPopup popup;
  std::string initialNote;
  AnnotationKind initialKind = AnnotationKind::Quote;
  bool saved = false;

  int listCount() const override;
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  bool handleCustomInput() override;
  static void provideRow(void* context, uint16_t index, freeink::ui::ListItem& item);
  void editNote();
  bool hasUnsavedChanges() const;
  void saveAnnotation();
  void confirmDelete();
  void showMessage(StrId message);
};
