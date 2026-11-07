#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "components/UiAppHost.h"

class GfxRenderer;
class MappedInputManager;

// Shared End-of-Book next-book menu for the EPUB and XTC readers. Collects up to
// MAX_SUGGESTIONS sibling books once per reader session, followed by Home.
class EndOfBookOptions : private UiAppHost {
 public:
  enum class Action { None, Redraw, OpenBook, GoHome, LastPage, BackToBook };

  static constexpr size_t MAX_SUGGESTIONS = 3;

  explicit EndOfBookOptions(GfxRenderer& renderer);

  // Scans the book's folder for suggestions; no-op when already loaded. Call ONLY from
  // the reader's render() (the render task, serialized by RenderLock) — the loaded flag
  // is the release/acquire publication point that lets the main task read the finished
  // list safely. `backToTitle` adds a leading "Back to <title>" row: the Quick Break
  // this book was opened from, so leaving early does not need the menu.
  void loadOnce(const std::string& currentBookPath, const std::string& backToTitle);

  // True when the end menu (including Home-only) owns touch/confirm/back input.
  bool menuActive() const;
  bool hasSuggestions() const { return menuActive() && !names.empty(); }

  // Menu input handling, following the standard list idiom: a tap on a row opens it
  // (or Home), side Up/Down and front Left/Right move the selection (wrapping),
  // Confirm opens the selection, and a short Back press returns to the last page of
  // the book. Fills openPath when the result is OpenBook. Returns Action::None when
  // nothing relevant was pressed; callers continue their normal input path (keeping
  // long-press Back to the file browser working).
  Action handleMenuInput(const MappedInputManager& input, std::string* openPath);

  // Draws the end screen with standard header/back chrome onto a cleared buffer.
  void render(GfxRenderer& renderer, const MappedInputManager& input);

 private:
  // The UiAppHost base hosts the suggestion list (themed rows, touch routing);
  // the title and button hints stay on the legacy UITheme calls.
  static void listScreen(UiScreen& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiScreen& screen);

  GfxRenderer& renderer;
  std::string folder;
  // Written by the render task in loadOnce(), immutable afterwards; the main task only
  // reads it after isLoaded is observed true (acquire), so no further locking is needed.
  std::vector<std::string> names;
  // Main-task selection updates may overlap a repaint on the render task.
  std::atomic<int> selector{0};
  freeink::ui::ListNav nav;
  std::atomic<bool> isLoaded{false};

  // Row storage, built once in loadOnce() (same acquire/release publication
  // point as names — see isLoaded above) rather than per-render in
  // buildListScreen(): names.size() is capped at MAX_SUGGESTIONS and never
  // changes afterward, so a fixed-capacity array avoids any heap allocation
  // for the row list, both at load time and every subsequent repaint.
  static constexpr size_t MAX_ROWS = MAX_SUGGESTIONS + 2;  // + "Back to ..." and Home
  std::string rowLabels[MAX_ROWS];
  freeink::ui::ListItem rowItems[MAX_ROWS]{};
  size_t rowCount = 0;
  // Leading Quick Break return row: its label and whether it exists.
  std::string backToLabel;
  bool hasBackRow = false;
  // Row index of the first suggested book (the back row shifts the list by one).
  int nameRowOffset() const { return hasBackRow ? 1 : 0; }
  void buildRowItems();

  // Row index dispatched by onRowEvent during the current route() call; -1 otherwise.
  int tappedRow = -1;

  std::string fullPath(size_t index) const;
  Action actionForRow(int row, std::string* openPath) const;
};
