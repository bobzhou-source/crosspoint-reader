#pragma once
#include <I18n.h>

#include <string>

#include "Boomerang.h"
#include "activities/UiListActivity.h"

// The TXT reader's menu: contents, page and percent jumps, frontlight and a
// screen refresh. Finishes with MenuResult{action}; the frontlight toggles in place.
class TxtReaderMenuActivity final : public UiListActivity {
 public:
  enum class MenuAction {
    CONTENTS,
    GO_TO_PAGE,
    GO_TO_PERCENT,
    FRONTLIGHT,
    REFRESH_SCREEN,
    READING_STATS,
    // Quick Break: open one unread Inbox item, or come back to the book a break
    // was taken from. The reader decides which by the live break state.
    QUICK_BREAK
  };

  // Reading position shown above the entries.
  struct Status {
    int page = 0;
    int pageCount = 0;
    int bookPercent = 0;
    bool indexing = false;  // the page count is an estimate until indexing completes
    int indexPercent = 100;
  };

  explicit TxtReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                 const Status& status, const boomerang::Row& breakRow = {});

  bool handleHomeGesture() override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
    // Row label override ("Back to <book>"); empty means the localized labelId.
    std::string label;
  };
  static constexpr size_t MAX_MENU_ITEMS = 8;
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
