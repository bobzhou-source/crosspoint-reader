#pragma once

#include <memory>

#include "AnnotationEntry.h"
#include "ReaderTextSlices.h"
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
  std::unique_ptr<AnnotationEntry> entry;
  char contextText[AnnotationEntry::MAX_EXCERPT_BYTES + 1] = {};
  size_t contextRowCount = 0;
  int16_t contextWidth = 0;
  freeink::ui::TextStyle contextStyle{};
  const freeink::ui::DrawTarget* contextTarget = nullptr;
  ReaderTextSlices::Cursor contextCursor;
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
