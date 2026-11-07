#pragma once
#include <I18n.h>

#include <string>

#include "activities/UiListActivity.h"

// The TXT reader's menu: contents, page and percent jumps, frontlight and a
// screen refresh. Finishes with MenuResult{action}; the frontlight toggles in place.
class TxtReaderMenuActivity final : public UiListActivity {
 public:
  enum class MenuAction { CONTENTS, GO_TO_PAGE, GO_TO_PERCENT, FRONTLIGHT, REFRESH_SCREEN, READING_STATS, DICTIONARY };

  // Reading position shown above the entries.
  struct Status {
    int page = 0;
    int pageCount = 0;
    int bookPercent = 0;
    bool indexing = false;  // the page count is an estimate until indexing completes
    int indexPercent = 100;
  };

  explicit TxtReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                 const Status& status);

  bool handleHomeGesture() override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };
  static constexpr size_t MAX_MENU_ITEMS = 7;
  MenuItem menuItems[MAX_MENU_ITEMS]{};
  size_t menuItemCount = 0;
  freeink::ui::ListItem menuRowItems[MAX_MENU_ITEMS]{};

  std::string title;
  Status status;

  int listCount() const override { return static_cast<int>(menuItemCount); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleButtons() override;
  void drawChrome() override;

  void closeCancelled();
};
