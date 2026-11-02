#pragma once
#include <HalStorage.h>
#include <TxtPagination.h>

#include <string>

#include "activities/UiListActivity.h"

// Contents of a TXT book: the chapter headings its page index has found so
// far, the current one marked. Finishes with PageResult{page} of the chosen
// heading. Titles are read from the book on demand, a window of rows at a
// time, so only heading offsets stay in RAM.
class TxtReaderChapterSelectionActivity final : public UiListActivity {
  std::string sourcePath;
  // The reader's index; it is not extended while this screen is on top.
  const TxtPagination::PageIndexBuilder& pageIndex;
  size_t currentOffset;
  int indexPercent;
  int currentChapter = -1;
  HalFile source;

  static constexpr int ROW_WINDOW = 24;
  static constexpr size_t TITLE_BYTES = 160;
  std::string windowLabels[ROW_WINDOW];
  char windowValues[ROW_WINDOW][12]{};
  freeink::ui::ListItem windowItems[ROW_WINDOW];
  int windowStart = -1;
  int windowCount = 0;
  void refreshRowWindow(int start);
  // The heading line at `offset`, trimmed; false when it cannot be read.
  bool readTitle(size_t offset, std::string& title);

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleButtons() override;
  void drawChrome() override;

 public:
  explicit TxtReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::string sourcePath, const TxtPagination::PageIndexBuilder& index,
                                             size_t currentOffset, int indexPercent);
  void onEnter() override;
  void onExit() override;
};
