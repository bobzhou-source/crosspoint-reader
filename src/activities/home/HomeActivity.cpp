#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "ReadingInboxActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/HabitBadge.h"
#include "util/SurpriseMeSource.h"

namespace {
StrId menuLabel(const HomeMenuItem item) {
  switch (item) {
    case HomeMenuItem::FILE_BROWSER:
      return StrId::STR_BROWSE_FILES;
    case HomeMenuItem::LIBRARY:
      return StrId::STR_LIBRARY;
    case HomeMenuItem::READING_INBOX:
      return StrId::STR_READING_INBOX;
    case HomeMenuItem::OPDS_BROWSER:
      return StrId::STR_OPDS_BROWSER;
    case HomeMenuItem::FILE_TRANSFER:
      return StrId::STR_FILE_TRANSFER;
    case HomeMenuItem::SETTINGS_MENU:
      return StrId::STR_SETTINGS_TITLE;
    case HomeMenuItem::HOME_SURPRISE:
      return StrId::STR_SURPRISE_ME;
    default:
      return StrId::STR_CONTINUE_READING;  // a recent-book row
  }
}

UIIcon menuIcon(const HomeMenuItem item) {
  switch (item) {
    case HomeMenuItem::FILE_BROWSER:
      return Folder;
    case HomeMenuItem::LIBRARY:
      return Library;
    case HomeMenuItem::READING_INBOX:
      return Book;
    case HomeMenuItem::OPDS_BROWSER:
      return Blocks;
    case HomeMenuItem::FILE_TRANSFER:
      return Transfer;
    case HomeMenuItem::SETTINGS_MENU:
      return Settings;
    case HomeMenuItem::HOME_SURPRISE:
      return Book;
    default:
      return Book;
  }
}
}  // namespace

int HomeActivity::getMenuItemCount() const {
  int count = 5;  // File Browser, Library, Reading inbox, File transfer, Settings
  if (!coverGridUi) count++;  // Surprise me (deal mode), list home only
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  return count;
}

HomeActivity::HomeLayout HomeActivity::homeLayout() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int endX = std::min(safe.x + safe.width, renderer.getScreenWidth() - right);
  const int endY = std::min(safe.y + safe.height, renderer.getScreenHeight() - bottom);
  safe.x = std::max(safe.x, left);
  safe.y = std::max(safe.y, top);
  safe.width = std::max(0, endX - safe.x);
  safe.height = std::max(0, endY - safe.y);
  const bool editorial = metrics.listSelectionStyle == 2;
  HomeLayout layout{};
  layout.rowHeight = GUI.getMenuRowHeight(renderer);
  layout.header = Rect{safe.x, safe.y + metrics.topPadding, safe.width,
                       editorial ? metrics.headerHeight : metrics.homeTopPadding - metrics.topPadding};
  const int coverHeight = recentBooks.empty() ? 144
      : editorial && recentBooks.size() == 1 ? metrics.homeCoverHeight + 32 : metrics.homeCoverTileHeight;
  layout.cover = Rect(safe.x, safe.y + metrics.homeTopPadding, safe.width, coverHeight);
  if (renderer.getScreenWidth() > renderer.getScreenHeight()) {
    layout.cover.width = safe.width / 2;
    layout.cover.height = std::min(layout.cover.height, std::max(0, endY - layout.cover.y - 16));
    layout.menu = Rect(safe.x + layout.cover.width, layout.cover.y, safe.width - layout.cover.width,
                       std::max(0, endY - layout.cover.y - 16));
    if (UITheme::isRtl()) {
      layout.menu.x = safe.x;
      layout.cover.x = safe.x + layout.menu.width;
    }
  } else {
    const int maxCoverHeight = std::max(0, endY - layout.cover.y - metrics.homeMenuTopOffset -
                                             layout.rowHeight - 16);
    layout.cover.height = std::min(layout.cover.height, maxCoverHeight);
    const int menuTop = layout.cover.y + layout.cover.height + metrics.homeMenuTopOffset;
    layout.menu = Rect(safe.x, menuTop, safe.width, std::max(0, endY - menuTop - 16));
  }
  layout.menuBase = metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size());
  const int count = getMenuItemCount() - layout.menuBase;
  layout.visibleRows = std::min(count, (layout.menu.height + metrics.menuSpacing) /
                                         (layout.rowHeight + metrics.menuSpacing));
  if (layout.visibleRows > 0) {
    const int selectedRow = std::max(0, selectorIndex - layout.menuBase);
    if (selectedRow < firstMenuRow || selectedRow >= firstMenuRow + layout.visibleRows) {
      firstMenuRow = (selectedRow / layout.visibleRows) * layout.visibleRows;
    }
    firstMenuRow = std::clamp(firstMenuRow, 0, std::max(0, count - layout.visibleRows));
    layout.firstRow = firstMenuRow;
  }
  return layout;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(coverGridUi ? maxBooks : std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::fillCoverGridFromLibrary() {
  if (recentBooks.size() >= CoverGridHomeUi::MAX_BOOKS) return;
  // Keep the index and record together off the task stack; reuse for every row.
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record;
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) {
    LOG_ERR("HOME", "OOM: library index");
    return;
  }
  auto& index = reader->index;
  auto& record = reader->record;
  // Never build here: on a first boot or after a lost index the walk would stall
  // the very first Home paint. Library builds in the background and Home picks
  // the index up on its next entry; until then the grid shows recents only.
  if (!index.open(library::libraryIndexPath())) {
    LOG_INF("HOME", "No library index yet; cover grid limited to recent books");
    return;
  }
  for (uint16_t row = 0; row < index.bookCount() && recentBooks.size() < CoverGridHomeUi::MAX_BOOKS; ++row) {
    RecentBook book;
    if (!index.readRecord(index.ordinalForRow(library::SortOrder::RecentDesc, row), record) ||
        !index.readPath(record, book.path))
      continue;
    if (std::any_of(recentBooks.begin(), recentBooks.end(),
                    [&](const RecentBook& existing) { return existing.path == book.path; }) ||
        RecentBooksStore::isMissing(book))
      continue;
    if (!index.readTitle(record, book.title) && !index.readName(record, book.title)) continue;
    index.readAuthor(record, book.author);
    if (index.ioFailed()) break;
    recentBooks.push_back(std::move(book));
  }
}

void HomeActivity::resolveGridCoverPaths() {
  for (auto& book : recentBooks) {
    if (!book.coverBmpPath.empty()) continue;
    // Constructors only derive cache paths; no metadata parsing or image generation.
    // Keep these large objects off the task stack and release each before the next book.
    if (FsHelpers::hasEpubExtension(book.path)) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (!epub) {
        LOG_ERR("HOME", "OOM: EPUB thumbnail path");
        continue;
      }
      book.coverBmpPath = epub->getThumbBmpPath();
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
      if (!xtc) {
        LOG_ERR("HOME", "OOM: XTC thumbnail path");
        continue;
      }
      book.coverBmpPath = xtc->getThumbBmpPath();
    }
  }
}

void HomeActivity::loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect) {
  if (!book.coverBmpPath.empty() && Storage.exists(UITheme::getCoverThumbPath(book.coverBmpPath, height).c_str()))
    return;
  // Only one parser lives at a time; EPUB/XTC objects exceed the stack budget.
  if (FsHelpers::hasEpubExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: cover EPUB");
      return;
    }
    book.coverBmpPath = epub->getThumbBmpPath();
    if (Storage.exists(epub->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (epub->generateThumbBmpFromSource(height)) {
      return;
    }
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: cover XTC");
      return;
    }
    book.coverBmpPath = xtc->getThumbBmpPath();
    if (Storage.exists(xtc->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (xtc->load() && xtc->generateThumbBmp(height)) {
      return;
    }
  }
  book.coverBmpPath.clear();
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    // The cover grid shares one slot size; generating at any other height
    // would rescale the dithered thumb at draw time and alias badly.
    const int thumbHeight = coverGridUi ? coverGridUi->thumbHeightFor() : coverHeight;
    if (coverGridUi) {
      loadGridCover(book, thumbHeight, showingLoading, popupRect);
      ++progress;
      if (showingLoading) GUI.fillPopupProgress(renderer, popupRect, progress * 100 / recentBooks.size());
      continue;
    }
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(thumbHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          freeCoverBuffer();
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(thumbHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            freeCoverBuffer();
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (UITheme::getInstance().hasCoverGridHome()) {
    // Screen-lifetime interaction tables and component properties exceed the stack budget.
    coverGridUi = makeUniqueNoThrow<CoverGridHomeUi>(renderer);
    if (!coverGridUi) LOG_ERR("HOME", "OOM: cover grid UI; using standard home");
  }
  loadRecentBooks(coverGridUi ? CoverGridHomeUi::MAX_BOOKS : metrics.homeRecentBooksCount);
  // Before the first paint, on the main task: the render task only reads this.
  habitBadge(habitText, sizeof(habitText));
  hasContinueReading = !recentBooks.empty();
  if (coverGridUi) {
    fillCoverGridFromLibrary();
    resolveGridCoverPaths();
    coverGridUi->begin(recentBooks, hasOpdsServers, hasContinueReading,
                       habitText[0] ? habitText : nullptr);
  }

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  coverGridUi.reset();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex, hasOpdsServers)) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::LIBRARY:
        onLibraryOpen();
        break;
      case HomeMenuItem::READING_INBOX:
        onReadingInboxOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:
        onOpdsBrowserOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      case HomeMenuItem::HOME_SURPRISE:
        onSurpriseMe();
        break;
      default:
        break;
    }
  };

  // Cover grid home splits navigation by button group (see below); the flat
  // next/previous cycle is for the classic list home only.
  if (!coverGridUi) {
    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && hasContinueReading && !recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
    return;
  }

  if (coverGridUi) {
    const int touched = coverGridUi->selectedAction(mappedInput);
    if (touched >= 0 && touched < menuCount) {
      selectorIndex = touched;
      activateSelection();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelection();
      return;
    }
    // Side page buttons walk the covers, front Left/Right walk the tabs
    // (selectorIndex is flat: books first, then the tab items). A press while
    // selection sits in the other band jumps into this band first.
    const int bookCount = static_cast<int>(recentBooks.size());
    const auto cycleBand = [this](const int base, const int count, const int dir) {
      if (count <= 0) return;
      int idx = selectorIndex - base;
      if (idx < 0 || idx >= count) {
        idx = dir > 0 ? 0 : count - 1;
      } else {
        idx = (idx + count + dir) % count;
      }
      selectorIndex = base + idx;
      requestUpdate();
    };
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, -1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, +1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [&cycleBand, bookCount, menuCount] {
      cycleBand(bookCount, menuCount - bookCount, -1);
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [&cycleBand, bookCount, menuCount] {
      cycleBand(bookCount, menuCount - bookCount, +1);
    });
    return;
  }

  const HomeLayout layout = homeLayout();
  const int recentCount = std::min(static_cast<int>(recentBooks.size()), metrics.homeRecentBooksCount);
  const int coverColumnWidth = (layout.cover.width - 2 * metrics.contentSidePadding) / std::max(1, recentCount);
  int touchedBook = -1;
  const auto coverTouch = mappedInput.colTouch(touchedBook, layout.cover.x + metrics.contentSidePadding,
                                               coverColumnWidth, recentCount, layout.cover.y,
                                               layout.cover.y + layout.cover.height, coverColumnWidth);
  if (coverTouch != MappedInputManager::RowTouch::None) {
    if (UITheme::isRtl()) touchedBook = recentCount - 1 - touchedBook;
    if (coverTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedBook) {
        selectorIndex = touchedBook;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedBook;
      activateSelection();
    }
    return;
  }

  int menuRow = -1;
  const auto menuTouch = mappedInput.rowTouch(
      menuRow, layout.menu.y, layout.rowHeight + metrics.menuSpacing, layout.visibleRows,
      layout.menu.x + metrics.contentSidePadding, layout.menu.x + layout.menu.width - metrics.contentSidePadding,
      layout.rowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex = layout.menuBase + layout.firstRow + menuRow;
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();
  if (coverGridUi) {
    coverGridUi->setSelection(selectorIndex);
    UITheme::getInstance().drawCoverGridHome(*coverGridUi);
    // Front Left/Right walk the tabs, so their hints read Left/Right; the
    // side page buttons (unhinted) walk the covers.
    const auto labels = mappedInput.mapLabels(hasContinueReading ? tr(STR_RESUME) : "", tr(STR_SELECT),
                                              tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH
                                                                   : HalDisplay::FAST_REFRESH);
    // Slot heights are recorded during the draw above; a change (first layout
    // pass, orientation switch) means the paths must point at those sizes and
    // any missing thumbs must be generated. Refreshing the paths right away
    // lets the next pass draw already-cached thumbs before generation runs.
    const bool coverSpecChanged = coverGridUi->takeThumbHeightChanged();
    if (coverSpecChanged) {
      coverGridUi->refreshCoverPaths();
      recentsLoaded = false;
    }
    if (!firstRenderDone) {
      firstRenderDone = true;
      requestUpdate();
    } else if (!recentsLoaded && !recentsLoading) {
      loadRecentCovers(CoverGridHomeUi::THUMB_HEIGHT);
      coverGridUi->refreshCoverPaths();
      requestUpdate();
    }
    return;
  }
  const HomeLayout layout = homeLayout();
  if (coverRectX != layout.cover.x || coverRectY != layout.cover.y || coverRectW != layout.cover.width ||
      coverRectH != layout.cover.height) {
    freeCoverBuffer();
    coverRendered = false;
  }
  coverRectX = layout.cover.x;
  coverRectY = layout.cover.y;
  coverRectW = layout.cover.width;
  coverRectH = layout.cover.height;
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  // Understated habit cue in the header's right label: the streak the reader is
  // protecting and what they read today. Absent until there is something to say.
  const char* habitText = this->habitText[0] ? this->habitText : nullptr;
  // Home is the stack root: no back button in its header.
  GUI.drawHeader(renderer, layout.header,
                 metrics.listSelectionStyle == 2 ? tr(STR_HOME_SHORTCUT)
                     : metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr,
                 habitText, false);
  GUI.drawRecentBookCover(renderer, layout.cover, recentBooks, selectorIndex, coverRendered, coverBufferStored,
                          bufferRestored, [this] { return storeCoverBuffer(); });
  if (metrics.listSelectionStyle == 2 && renderer.getScreenWidth() <= renderer.getScreenHeight()) {
    const int ruleY = layout.menu.y - metrics.homeMenuTopOffset / 2;
    renderer.drawLine(layout.cover.x + metrics.contentSidePadding, ruleY,
                      layout.cover.x + layout.cover.width - metrics.contentSidePadding - 1, ruleY);
  }

  // Labels and icons come from the menu ITEM, not the row position, so the
  // optional OPDS row and the appended deal-mode row cannot shift each other.
  const auto menuEntry = [this, &layout](const int row) {
    return indexToMenuItem(layout.menuBase + layout.firstRow + row - static_cast<int>(recentBooks.size()),
                           hasOpdsServers);
  };
  GUI.drawButtonMenu(renderer, layout.menu, layout.visibleRows, selectorIndex - layout.menuBase - layout.firstRow,
                     [&menuEntry](int row) { return std::string(I18N.get(menuLabel(menuEntry(row)))); },
                     [&menuEntry](int row) { return menuIcon(menuEntry(row)); });
  const int rowCount = getMenuItemCount() - layout.menuBase;
  if (layout.visibleRows > 0 && rowCount > layout.visibleRows) {
    const int trackHeight = layout.visibleRows * (layout.rowHeight + metrics.menuSpacing) - metrics.menuSpacing;
    const int thumbHeight = std::min(trackHeight, std::max(44, trackHeight * layout.visibleRows / rowCount));
    const int thumbY = layout.menu.y + (trackHeight - thumbHeight) * layout.firstRow /
                                         (rowCount - layout.visibleRows);
    const int barX = UITheme::isRtl() ? layout.menu.x + metrics.scrollBarRightOffset
        : layout.menu.x + layout.menu.width - metrics.scrollBarRightOffset - metrics.scrollBarWidth;
    renderer.drawLine(barX, layout.menu.y, barX, layout.menu.y + trackHeight - 1);
    renderer.fillRect(barX, thumbY, metrics.scrollBarWidth, thumbHeight);
  }

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (notice) {
    // One-shot feedback for a deal-mode press that found nothing to open. The
    // next repaint (navigation or the reader) removes it; no second refresh is
    // requested, so it does not cost an extra panel update.
    GUI.drawPopup(renderer, notice);
    notice = nullptr;
  }

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    const int themeThumbHeight = GUI.homeCoverThumbHeight(renderer, static_cast<int>(recentBooks.size()));
    loadRecentCovers(themeThumbHeight > 0 ? themeThumbHeight : metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

// Deal mode: one press, one unread item, straight into the reader. The reader
// opens at the saved position (page 1 for a book that was never started), so no
// extra screen or confirmation stands between the press and the text.
void HomeActivity::onSurpriseMe() {
  std::string path;
  if (!surprise::findBook(path)) {
    notice = tr(STR_SURPRISE_NONE);
    requestUpdate();
    return;
  }
  onSelectBook(path);
}

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onLibraryOpen() { activityManager.goToLibrary(); }

void HomeActivity::onReadingInboxOpen() {
  auto inbox = makeUniqueNoThrow<ReadingInboxActivity>(renderer, mappedInput);
  if (!inbox) {
    LOG_ERR("HOME", "OOM: reading inbox");
    return;
  }
  activityManager.replaceActivity(std::move(inbox));
}

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
