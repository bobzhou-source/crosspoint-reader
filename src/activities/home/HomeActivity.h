#pragma once
#include <atomic>
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/CoverGridHomeUi.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

struct Rect;

class HomeActivity final : public Activity {
  std::unique_ptr<CoverGridHomeUi> coverGridUi;
  ButtonNavigator buttonNavigator;
  int firstMenuRow = 0;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool hasContinueReading = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  const char* notice = nullptr;    // one-shot text drawn by the next render
  // Habit line, snapshotted in onEnter: the render task must not touch the stats
  // accumulator (it may need to read the card on first use).
  char habitText[64] = {};
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;
  // A Library transition may prepare private data during the blocking waveform,
  // but never while Home draws/updates recents or performs post-frame cover I/O.
  enum class NavigationFramePhase : uint8_t { Idle, Drawing, Waveform, Frozen };
  std::atomic<NavigationFramePhase> navigationFramePhase{NavigationFramePhase::Idle};
  std::atomic<uint32_t> feedbackRequested{0};
  std::atomic<uint32_t> feedbackPrepared{0};

  // Convert HomeMenuItem to menu index (used in onEnter)
  int menuItemToIndex(HomeMenuItem item, bool hasOpdsUrl) const {
    int i = 0;
    if (item == (coverGridUi ? HomeMenuItem::FILE_BROWSER : HomeMenuItem::LIBRARY)) return i;
    ++i;
    if (item == (coverGridUi ? HomeMenuItem::LIBRARY : HomeMenuItem::READING_INBOX)) return i;
    ++i;
    if (item == (coverGridUi ? HomeMenuItem::READING_INBOX : HomeMenuItem::FILE_BROWSER)) return i;
    ++i;
    if (item == HomeMenuItem::OPDS_BROWSER) return hasOpdsUrl ? i : 0;
    if (hasOpdsUrl) ++i;
    if (item == HomeMenuItem::FILE_TRANSFER) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    ++i;
    // The deal-mode entry is the last row and only exists on the list home;
    // the cover-grid home reaches it through its own tab strip.
    if (item == HomeMenuItem::HOME_SURPRISE) return i;
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  HomeMenuItem indexToMenuItem(int idx, bool hasOpdsUrl) const {
    int i = 0;
    if (idx == i++) return coverGridUi ? HomeMenuItem::FILE_BROWSER : HomeMenuItem::LIBRARY;
    if (idx == i++) return coverGridUi ? HomeMenuItem::LIBRARY : HomeMenuItem::READING_INBOX;
    if (idx == i++) return coverGridUi ? HomeMenuItem::READING_INBOX : HomeMenuItem::FILE_BROWSER;
    if (hasOpdsUrl && idx == i++) return HomeMenuItem::OPDS_BROWSER;
    if (idx == i++) return HomeMenuItem::FILE_TRANSFER;
    if (idx == i++) return HomeMenuItem::SETTINGS_MENU;
    if (!coverGridUi && idx == i) return HomeMenuItem::HOME_SURPRISE;
    return HomeMenuItem::NONE;
  }
  void onSelectBook(const std::string& path);
  void onSurpriseMe();
  void onFileBrowserOpen();
  void onLibraryOpen();
  void onReadingInboxOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();

  int getMenuItemCount() const;
  struct HomeLayout {
    Rect header;
    Rect cover;
    Rect menu;
    int menuBase;
    int firstRow;
    int visibleRows;
    int rowHeight;
  };
  HomeLayout homeLayout();
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);
  void fillCoverGridFromLibrary();
  void resolveGridCoverPaths();
  void loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Called only for a committed Home→Library replace, before RenderLock.
  // False means no classic touch feedback was queued; never waits in that case.
  bool prepareLibraryTransition();
  bool isHomeActivity() const override { return true; }
};
