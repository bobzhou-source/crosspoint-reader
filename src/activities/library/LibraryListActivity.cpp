#include "LibraryListActivity.h"

#include <Arduino.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/icons/headerIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/SurpriseMeSource.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long LONG_PRESS_MS = 1000;

constexpr int RECENT_TAB = 0;
constexpr int TITLE_TAB = 1;
constexpr int AUTHOR_TAB = 2;
constexpr int TAB_SLOTS = AUTHOR_TAB + 1;

constexpr bool isDescending(const library::SortOrder order) {
  return order == library::SortOrder::RecentDesc || order == library::SortOrder::TitleDesc ||
         order == library::SortOrder::AuthorDesc;
}

constexpr bool isRecentSort(const library::SortOrder order) {
  return order == library::SortOrder::RecentAsc || order == library::SortOrder::RecentDesc;
}

constexpr bool isAuthorSort(const library::SortOrder order) {
  return order == library::SortOrder::AuthorAsc || order == library::SortOrder::AuthorDesc;
}

constexpr library::SortOrder orderForTab(const int tab, const uint8_t descendingTabs) {
  const bool descending = (descendingTabs & (1u << tab)) != 0;
  if (tab == TITLE_TAB) return descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  if (tab == AUTHOR_TAB) return descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
  return descending ? library::SortOrder::RecentDesc : library::SortOrder::RecentAsc;
}

const char* tabLabelFor(const int tab) {
  if (tab == TITLE_TAB) return tr(STR_LIBRARY_TAB_TITLE);
  if (tab == AUTHOR_TAB) return tr(STR_LIBRARY_TAB_AUTHOR);
  return tr(STR_LIBRARY_TAB_RECENT);
}

// Background index refresh. Priority 1 is the loop and render tasks' level
// (ReaderSync's worker runs there too); the walk blocks on the card constantly,
// so it yields to both. The stack is an ordinary internal-heap stack like
// ReaderSync's: no task in this codebase uses a PSRAM stack.
#ifdef BOARD_HAS_PSRAM
constexpr uint32_t BUILD_TASK_STACK_BYTES = 16384;
#else
constexpr uint32_t BUILD_TASK_STACK_BYTES = 12288;
#endif
constexpr UBaseType_t BUILD_TASK_PRIORITY = 1;
// E-ink repaints for the progress counters: rare while a shelf is browsable,
// a little more often on the blank progress screen.
constexpr uint32_t BUILD_REPAINT_BROWSABLE_MS = 5000;
constexpr uint32_t BUILD_REPAINT_BLANK_MS = 2000;
// The task normally starts once the first paint has finished; if that never
// signals, start anyway after this long.
constexpr uint32_t BUILD_START_FALLBACK_MS = 5000;
// Automatic follow-up refreshes per visit when a transfer re-dirtied the index
// during a walk. Bounded so a stuck dirty marker cannot spin the card.
constexpr uint8_t BUILD_MAX_FOLLOWUPS = 2;
}  // namespace

// Shared between the loop task (owner) and the worker. The worker only writes
// these fields and sets `done` last; see the contract in the header.
struct LibraryBuildJob {
  std::atomic<uint16_t> scanned{0};
  std::atomic<uint16_t> parsed{0};
  std::atomic<uint16_t> reused{0};
  std::atomic<bool> cancel{false};
  std::atomic<bool> done{false};
  bool readMetadata = false;
  bool ok = false;
  library::BuildStats stats;
};

namespace {
// Called on the worker between work units, never under a RenderLock.
bool serviceBuild(void* context, const uint16_t scanned, const uint16_t parsed, const uint16_t reused) {
  auto* job = static_cast<LibraryBuildJob*>(context);
  job->scanned.store(scanned, std::memory_order_relaxed);
  job->parsed.store(parsed, std::memory_order_relaxed);
  job->reused.store(reused, std::memory_order_relaxed);
  return !job->cancel.load(std::memory_order_relaxed);
}

void buildTaskEntry(void* arg) {
  auto* job = static_cast<LibraryBuildJob*>(arg);
  {
    HalPowerManager::Lock power;  // full CPU speed for the walk
    library::BuildControl control;
    control.context = job;
    control.service = &serviceBuild;
    control.deferInstall = true;  // publish from the UI task, under RenderLock
    job->ok = library::buildLibraryIndex("/", job->stats, job->readMetadata, &control);
  }
  // Last access to the job: the owner frees it as soon as it observes this.
  job->done.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}
}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Library", renderer, mappedInput, true) {}

// A worker still running here would outlive the job it points at; onExit()
// normally joined it already, so this is the backstop.
LibraryListActivity::~LibraryListActivity() { stopBuild(); }

void LibraryListActivity::onEnter() {
  // One lock across the base lifecycle AND the data phase: the base onEnter
  // schedules a paint, and the render task must not read the index or the
  // filter before they are in place.
  RenderLock lock(*this);
  UiTabListActivity::onEnter();
  app.on(ACTION_SEARCH, &LibraryListActivity::searchActionTrampoline, this);
  app.on(ACTION_REBUILD, &LibraryListActivity::rebuildActionTrampoline, this);
  app.on(ACTION_BACK, &LibraryListActivity::backActionTrampoline, this);

  // Recent is backed by the resident store. Prune before opening the index so
  // its persistence write never overlaps the long-lived index reader.
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();

  // Paint from whatever index is on the card right now. A missing, dirty or
  // other-metadata-mode index is refreshed by a background task that starts
  // after the first paint (pollBuild); an existing one stays browsable.
  const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
  const bool opened = index.open(library::libraryIndexPath());
  buildPending = !opened || library::isLibraryIndexDirty() || index.header().metadataEnabled != readMetadata;
  if (!opened) index.close();
  buildFailed = false;
  buildHeld = false;
  buildRounds = 0;
  enteredMs = millis();
  firstPainted.store(false, std::memory_order_release);
  syncRebuildFlag();
  degraded = index.isOpen() && index.ranksDegraded();
  if (index.isOpen() && index.dedupDegraded()) {
    LOG_ERR("LIB", "index was built without duplicate detection");
  }
  resolvePinned();

  // Entered while Confirm was still held (typical when launched from the home
  // menu): ignore its release, or we would open whatever sits at row 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  stopBuild();  // join before the activity (and the job) go away
  index.close();
  Activity::onExit();
}

void LibraryListActivity::syncRebuildFlag() {
  rebuildActive.store(buildPending || build != nullptr, std::memory_order_relaxed);
}

bool LibraryListActivity::rebuildStatusText(char* out, const size_t size) const {
  if (!rebuildActive.load(std::memory_order_relaxed)) return false;
  const unsigned scanned = progressScanned.load(std::memory_order_relaxed);
  const unsigned done = progressDone.load(std::memory_order_relaxed);
  if (scanned == 0) {
    snprintf(out, size, "%s", tr(STR_LIBRARY_REBUILDING));
  } else if (done == 0) {
    snprintf(out, size, "%s %u", tr(STR_LIBRARY_REBUILDING), scanned);
  } else {
    snprintf(out, size, "%s %u (%u)", tr(STR_LIBRARY_REBUILDING), scanned, done);
  }
  return true;
}

// Manual refresh and post-delete refresh: queue it; pollBuild starts it. A
// refresh already queued or running covers the request.
void LibraryListActivity::queueRebuild() {
  if (build || buildPending) return;
  buildFailed = false;
  buildRounds = 0;
  buildPending = true;
  syncRebuildFlag();
  requestUpdate();
}

void LibraryListActivity::startBuild() {
  if (build) return;
  auto job = makeUniqueNoThrow<LibraryBuildJob>();
  if (job) {
    job->readMetadata = SETTINGS.libraryUseMetadata != 0;
    if (xTaskCreate(&buildTaskEntry, "LibraryBuild", BUILD_TASK_STACK_BYTES, job.get(), BUILD_TASK_PRIORITY, nullptr) ==
        pdPASS) {
      // The worker may already be running; it only touches `job`, which this
      // unique_ptr keeps alive and unmoved in memory.
      build = std::move(job);
      buildPending = false;
      progressScanned.store(0, std::memory_order_relaxed);
      progressDone.store(0, std::memory_order_relaxed);
      paintedScanned = 0;
      lastStatusPaintMs = millis();
      syncRebuildFlag();
      return;
    }
  }
  LOG_ERR("LIB", "cannot start index refresh task");
  buildPending = false;
  buildFailed = true;
  syncRebuildFlag();
  requestUpdate();
}

// Cancel and join. The builder polls the cancel flag between work units, so
// the wait is bounded by one unit (at worst a single book's metadata parse);
// when it returns the worker has made its last SdFat call and is only
// deleting itself. A build that already finished is discarded too: installing
// it here could reorder rows under an action that is about to read them. The
// refresh stays queued and its journal keeps the metadata already parsed.
void LibraryListActivity::stopBuild() {
  if (!build) return;
  build->cancel.store(true, std::memory_order_relaxed);
  while (!build->done.load(std::memory_order_acquire)) vTaskDelay(1);
  build.reset();
  buildPending = true;
  progressScanned.store(0, std::memory_order_relaxed);
  progressDone.store(0, std::memory_order_relaxed);
  syncRebuildFlag();
}

void LibraryListActivity::holdBuild() {
  buildHeld = true;
  stopBuild();
}

void LibraryListActivity::cancelBuildAsync() {
  if (build) build->cancel.store(true, std::memory_order_relaxed);
}

// Top of loop(): the only place the index file is swapped, so button handling
// later in the same pass always sees one consistent index.
void LibraryListActivity::pollBuild() {
  if (!build) {
    if (!buildPending || buildHeld) return;
    if (!firstPainted.load(std::memory_order_acquire) && millis() - enteredMs < BUILD_START_FALLBACK_MS) return;
    startBuild();
    return;
  }

  const uint32_t scanned = build->scanned.load(std::memory_order_relaxed);
  progressScanned.store(scanned, std::memory_order_relaxed);
  progressDone.store(static_cast<uint32_t>(build->parsed.load(std::memory_order_relaxed)) +
                         build->reused.load(std::memory_order_relaxed),
                     std::memory_order_relaxed);
  if (!build->done.load(std::memory_order_acquire)) {
    const uint32_t interval = index.isOpen() ? BUILD_REPAINT_BROWSABLE_MS : BUILD_REPAINT_BLANK_MS;
    if (scanned != paintedScanned && millis() - lastStatusPaintMs >= interval) {
      paintedScanned = scanned;
      lastStatusPaintMs = millis();
      requestUpdate();
    }
    return;
  }
  // A finished build waits while a row menu is up: swapping the index under it
  // would reorder the rows it was opened on.
  if (optionPopup.isActive()) return;
  completeBuild();
}

void LibraryListActivity::completeBuild() {
  const bool cancelled = build->cancel.load(std::memory_order_relaxed);
  const bool ok = build->ok;
  const library::BuildStats stats = build->stats;
  build.reset();
  progressScanned.store(0, std::memory_order_relaxed);
  progressDone.store(0, std::memory_order_relaxed);
  if (cancelled) {  // Back pressed: the screen is already on its way out
    buildPending = true;
    syncRebuildFlag();
    return;
  }
  if (!ok) {
    LOG_ERR("LIB", "index refresh failed");
    buildPending = false;
    buildFailed = true;  // the old index, if any, stays; no automatic retry
    syncRebuildFlag();
    requestUpdate();
    return;
  }

  LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed, %u enriched (%u dup, %u unreadable)",
          static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
          static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
          static_cast<unsigned>(stats.enriched), static_cast<unsigned>(stats.duplicatesDropped),
          static_cast<unsigned>(stats.unreadableSkipped));
  if (stats.dedupDegraded) LOG_ERR("LIB", "rebuild completed without duplicate detection");

  {
    // The render task must not read the index (or the filter) while the file is
    // swapped, and the install renames over the file the reader has open.
    RenderLock lock(*this);
    const bool wasOpen = index.isOpen();
    const uint16_t previousCount = index.bookCount();
    index.close();
    if (stats.indexPrepared && !library::installPreparedLibraryIndex()) {
      LOG_ERR("LIB", "cannot publish the rebuilt index; keeping the previous one");
    }
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
    degraded = index.isOpen() && index.ranksDegraded();
    // Rows only move when the book set or its text changed. An unchanged
    // refresh keeps the selection, viewport, filter and pins exactly as they
    // are; a changed one clamps them like any other rebuild.
    const bool changed = stats.indexPrepared || !wasOpen || previousCount != index.bookCount();
    if (changed) {
      resetAfterRebuild();
      // The published interaction table still names rows in the old order.
      // Do not let a pending tap/Confirm open a different book before repaint.
      closeRouting();
      firstPainted.store(false, std::memory_order_release);
      enteredMs = millis();
    }
  }

  buildFailed = !index.isOpen();
  // A transfer finished during the walk re-marks the index dirty; one more
  // pass picks it up, but never loop on a marker that will not clear.
  buildPending = !buildFailed && library::isLibraryIndexDirty() && buildRounds < BUILD_MAX_FOLLOWUPS;
  if (buildPending) buildRounds++;
  syncRebuildFlag();
  requestUpdate(true);
}

void LibraryListActivity::swallowHeldReleases() {
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

int LibraryListActivity::selectedEntry() const {
  const int entry = ringPos() - 1;
  return entry < 0 ? 0 : entry;
}

// The pinned overlay applies only to the shelf that reads as "what am I up
// to": the unfiltered Recent sort, newest first. A search result is a flat
// list the reader narrowed down on purpose, and the ascending toggle asks for
// oldest-first, which pinned fresh reads would contradict.
int LibraryListActivity::pinnedCount() const {
  if (activeTabIndex != RECENT_TAB || !query.empty() || !isDescending(sortOrder)) return 0;
  return pinnedTotal;
}

void LibraryListActivity::resolvePinned() {
  const auto& books = RECENT_BOOKS.getBooks();
  pinnedTotal = static_cast<uint8_t>(std::min<size_t>(books.size(), RecentBooksStore::MAX_RECENT_BOOKS));
  for (int i = 0; i < pinnedTotal; i++) pinnedAscRows[i] = 0xFFFF;
  if (pinnedTotal > 0 && index.isOpen()) {
    library::BookIdentity identities[RecentBooksStore::MAX_RECENT_BOOKS];
    for (int i = 0; i < pinnedTotal; i++) {
      const std::string& path = books[static_cast<size_t>(i)].path;
      identities[i].pathHash = library::clixPathHash(path.data(), path.size());
      // Size is only a lookup prefilter; 0 (stat failed, e.g. the index handle
      // is the card's one open reader) falls back to hash-only matching.
      identities[i].fileSize = 0;
      HalFile file;
      if (Storage.openFileForRead("LIB", path.c_str(), file)) {
        identities[i].fileSize = static_cast<uint32_t>(file.fileSize());
      }
    }
    if (!index.recentRowsFor(identities, pinnedTotal, pinnedAscRows)) {
      // Without the match the overlay would duplicate every pinned book that is
      // also in the index; better to drop the pins than to show doubles.
      LOG_ERR("LIB", "recent-book lookup failed; overlay disabled");
      pinnedTotal = 0;
    }
  }
  refreshOverlap();
}

void LibraryListActivity::refreshOverlap() {
  overlapCount = 0;
  const int total = static_cast<int>(index.bookCount());
  for (int i = 0; i < pinnedTotal; i++) {
    if (pinnedAscRows[i] == 0xFFFF || pinnedAscRows[i] >= total) continue;
    const uint16_t row =
        isDescending(sortOrder) ? static_cast<uint16_t>(total - 1 - pinnedAscRows[i]) : pinnedAscRows[i];
    overlapRows[overlapCount++] = row;
  }
  std::sort(overlapRows, overlapRows + overlapCount);
}

void LibraryListActivity::openSelectedBook() {
  std::string path;
  if (selectedEntry() < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (selectedEntry() >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(selectedEntry())].path;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(selectedEntry())));
    if (ordinal == 0xFFFF) return;

    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
      LOG_ERR("LIB", "cannot resolve path for row %d", selectedEntry());
      return;
    }
  }
  openBookByPath(path);
}

// Shared by row activation and the options menu: the reader screen this opens
// has its own surfaces; a lingering tap flash would gray an unrelated element
// there. Release the worker and index before the reader claims their memory
// and begins its own card access.
void LibraryListActivity::openBookByPath(const std::string& path) {
  app.clearTapFlash();
  stopBuild();  // the reader needs the card and the heap to itself
  index.close();
  onSelectBook(path);
}

void LibraryListActivity::activateIndex(const int index) {
  if (groupsCollapsed) {
    expandGroup(index);
  } else {
    openSelectedBook();
  }
}

// Row long-press prompts delete wherever grouping does not own the gesture:
// an active search is already a flat list the reader narrowed down on purpose
// ("find it, hold it, delete it"). Unfiltered Title/Author lists keep
// collapse-to-groups. The Recent shelf always opens the row options menu.
bool LibraryListActivity::deleteEligible() const { return !groupsCollapsed && (!query.empty() || !groupable()); }

void LibraryListActivity::onRowLongPress(const int index) {
  if (isRecentSort(sortOrder)) {
    showRecentBookOptions(index);
  } else if (deleteEligible()) {
    promptDeleteBook(index);
  } else if (!groupsCollapsed && groupable()) {
    collapseGroups(index);
  } else {
    activateIndex(index);
  }
}

// Recent-shelf long-press menu (button hold and touch long-press). The first
// rows may come from RecentBooksStore; the rest are index rows sorted by
// modification time. Only store rows can be removed from recents.
void LibraryListActivity::showRecentBookOptions(const int entry) {
  if (entry < 0 || entry >= listCount()) return;

  std::string path;
  std::string title;
  const bool isStoreRow = entry < pinnedCount();
  if (isStoreRow) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(entry)].path;
    title = books[static_cast<size_t>(entry)].title;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    std::string author;
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path) ||
        !rowTextFor(entry, title, author)) {
      LOG_ERR("LIB", "cannot resolve Recent row %d", entry);
      return;
    }
  }

  const char* STORE_OPTIONS[] = {tr(STR_OPEN), tr(STR_SURPRISE_ME), tr(STR_REMOVE_FROM_RECENTS), tr(STR_DELETE),
                                 tr(STR_LIBRARY_REBUILD)};
  const char* INDEX_OPTIONS[] = {tr(STR_OPEN), tr(STR_SURPRISE_ME), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY), title.c_str(), isStoreRow ? STORE_OPTIONS : INDEX_OPTIONS, isStoreRow ? 5 : 4, 0,
                   [this, path, title, isStoreRow](const int choice) {
                     swallowHeldReleases();
                     switch (choice) {
                       case 0:
                         openBookByPath(path);
                         break;
                       case 1:
                         openSurprise();
                         break;
                       case 2:
                         if (isStoreRow) {
                           promptRemoveRecentBook(path, title);
                         } else {
                           promptDeleteBookByPath(path, title);
                         }
                         break;
                       case 3:
                         if (isStoreRow)
                           promptDeleteBookByPath(path, title);
                         else
                           queueRebuild();
                         break;
                       case 4:
                         if (isStoreRow) queueRebuild();
                         break;
                       default:
                         break;
                     }
                   });
  requestUpdate();
}

// Deal mode from the shelf: pick something unread and open it, so a reader who
// cannot choose does not have to. The reader needs the card and the heap, so the
// worker and the index handle are released first, exactly as for a row open.
void LibraryListActivity::openSurprise() {
  app.clearTapFlash();
  optionPopup.dismiss();
  stopBuild();
  index.close();
  std::string path;
  if (!surprise::findBook(path)) {
    // Never fall back to a finished book: the shelf stays usable and says why.
    static constexpr StrId OK[] = {StrId::STR_OK_BUTTON};
    optionPopup.show(StrId::STR_SURPRISE_ME, OK, 1, 0, [this](int) { swallowHeldReleases(); });
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    requestUpdate();
    return;
  }
  onSelectBook(path);
}

void LibraryListActivity::resetAfterRebuild() {
  // Sort positions, group starts, and pinned rows all point into the old order.
  applyFilter();
  resolvePinned();
  auto& nav = activeNav();
  const int count = listCount();
  if (count == 0) {
    nav.selected = 0;
  } else if (nav.selected > count) {
    nav.selected = count;
  }
  nav.followOnBuild = true;
}

void LibraryListActivity::promptRemoveRecentBook(const std::string& path, const std::string& title) {
  holdBuild();
  const bool reopenIndex = index.isOpen();
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: recent removal confirmation");
    buildHeld = false;
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path, reopenIndex](const ActivityResult& result) {
    buildHeld = false;
    swallowHeldReleases();
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    if (!result.isCancelled && RECENT_BOOKS.removeByPath(path)) {
      resolvePinned();
      closeRouting();
      auto& nav = activeNav();
      const int count = listCount();
      if (count == 0) {
        nav.selected = 0;
      } else if (nav.selected > count) {
        nav.selected = count;
      }
      nav.followOnBuild = true;
    }
  });
}

void LibraryListActivity::promptDeleteBook(const int entry) {
  if (!index.isOpen() || entry < 0 || entry >= bookRowCount()) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  if (ordinal == 0xFFFF) return;

  std::string path;
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", entry);
    return;
  }
  std::string title;
  std::string author;
  rowTextFor(entry, title, author);
  promptDeleteBookByPath(path, title);
}

void LibraryListActivity::promptDeleteBookByPath(const std::string& path, const std::string& title) {
  // The dialog and the delete both want the card; reopen when we resume.
  holdBuild();
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE) + std::string("? "), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: delete confirmation");
    buildHeld = false;
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    buildHeld = false;
    swallowHeldReleases();
    {
      // The render task must not read the index (or the filter) around the
      // deletion and the reopen.
      RenderLock lock(*this);
      if (!result.isCancelled) {
        LOG_DBG("LIB", "deleting %s", path.c_str());
        clearBookCache(path);
        if (!Storage.remove(path.c_str())) LOG_ERR("LIB", "cannot delete %s", path.c_str());
        if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
        // Refresh in the background; the reopened index still lists the book
        // until it completes. The marker makes an interrupted refresh resume.
        library::markLibraryIndexDirty();
        queueRebuild();
      }
      if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
      if (!result.isCancelled) {
        resetAfterRebuild();
      }
    }
    if (!result.isCancelled) {
      closeRouting();
      requestUpdate(true);
    }
  });
}

void LibraryListActivity::openSearch() {
  if (!index.isOpen()) return;
  app.clearTapFlash();
  // No key filtering here on purpose. Greying out the letters that lead nowhere
  // was built, tested on device and removed: a letter you can see but cannot
  // reach reads as a broken keyboard, and the eye keeps returning to it.
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query, 48,
                                                           InputType::Text);
  if (!keyboard) {
    LOG_ERR("LIB", "OOM: search keyboard");
    return;
  }
  holdBuild();
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    buildHeld = false;
    swallowHeldReleases();
    if (result.isCancelled) return;
    query = std::get<KeyboardResult>(result.data).text;
    applyFilter();
    auto& nav = activeNav();
    if (!query.empty() && filteredCount == 0 && !degraded) {
      // Up from the tab bar reopens Search even with no results.
      nav.selected = 0;
    } else {
      // A non-empty result belongs to the list: land on
      // its first surviving row, not on the strip.
      nav.selected = 1;
    }
    nav.top = 0;
    requestUpdate();
  });
}

void LibraryListActivity::stepTab(const int direction) {
  const int next = (activeTab() + (direction > 0 ? 1 : TAB_SLOTS - 1)) % TAB_SLOTS;
  selectTab(next, false);
}

void LibraryListActivity::onTabAction(const int index) {
  app.clearTapFlash();
  selectTab(index, true);
}

void LibraryListActivity::selectTab(const int index, const bool toggleIfActive) {
  if (index < 0 || index >= TAB_SLOTS) return;
  if (toggleIfActive && index == activeTab()) descendingTabs ^= static_cast<uint8_t>(1u << index);
  sortOrder = orderForTab(index, descendingTabs);
  // The filter and the overlap rows hold positions in the old order, so they
  // must be rebuilt.
  applyFilter();
  activeTabIndex = index;
  refreshOverlap();
  // Tab changes happen only while the bar owns focus. A tab's remembered row
  // must not pull focus back into the list after the switch.
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
  requestUpdate();
}

void LibraryListActivity::toggleSortDirection() { selectTab(activeTab(), true); }

int LibraryListActivity::tabCount() const { return TAB_SLOTS; }

int LibraryListActivity::activeTab() const { return activeTabIndex; }

const char* LibraryListActivity::tabLabel(const int index) const { return tabLabelFor(index); }

fui::TabIndicator LibraryListActivity::tabIndicator(const int index) const {
  if (index != activeTab()) return fui::TabIndicator::None;
  return isDescending(sortOrder) ? fui::TabIndicator::Down : fui::TabIndicator::Up;
}

int LibraryListActivity::bookRowCount() const {
  if (!query.empty()) return static_cast<int>(filteredCount);
  // Pinned books already in the index are skipped below the pins, not doubled;
  // pinned books the index missed still show, so the difference stays split.
  const int pinned = pinnedCount();
  return static_cast<int>(index.bookCount()) + (pinned > 0 ? pinned - overlapCount : 0);
}

int LibraryListActivity::listCount() const { return groupsCollapsed ? static_cast<int>(groupCount) : bookRowCount(); }

// Entry position on screen to row position in the sort order. Identity while
// unfiltered and unpinned, so the shelf costs nothing when nothing is typed.
// With pins active, entries below pinnedCount() belong to the store and must
// not reach this; the rest walk past the pinned books' own sort rows.
int LibraryListActivity::rowFor(const int entry) const {
  if (!query.empty()) {
    if (entry < 0 || entry >= static_cast<int>(filteredCount) || !filtered) return 0;
    return filtered[entry];
  }
  const int pinned = pinnedCount();
  if (pinned == 0) return entry;
  int row = entry - pinned;
  for (int i = 0; i < overlapCount; i++) {
    if (overlapRows[i] <= row) row++;
  }
  return row;
}

bool LibraryListActivity::groupable() const { return !degraded && !isRecentSort(sortOrder) && bookRowCount() > 0; }

uint32_t LibraryListActivity::titleInitialFor(const int entry) {
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return 0;
  return library::foldedGroupInitial(std::string_view(record.fold, record.foldLen));
}

bool LibraryListActivity::buildGroupStarts() {
  const int count = bookRowCount();
  if (count <= 0) return false;
  if (groupCapacity < count) {
    auto starts = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(count));
    if (!starts) {
      LOG_ERR("LIB", "cannot allocate %u-byte group map", static_cast<unsigned>(count * sizeof(uint16_t)));
      return false;
    }
    groupStarts = std::move(starts);
    groupCapacity = static_cast<uint16_t>(count);
  }

  groupCount = 0;
  uint32_t previousInitial = 0;
  std::string previousAuthor;
  std::string title;
  std::string author;
  previousAuthor.reserve(128);
  title.reserve(128);
  author.reserve(128);
  for (int entry = 0; entry < count; entry++) {
    bool startsGroup = entry == 0;
    if (isAuthorSort(sortOrder)) {
      rowTextFor(entry, title, author);
      startsGroup = startsGroup || author != previousAuthor;
      previousAuthor = author;
    } else {
      const uint32_t initial = titleInitialFor(entry);
      startsGroup = startsGroup || initial != previousInitial;
      previousInitial = initial;
    }
    if (startsGroup) groupStarts[groupCount++] = static_cast<uint16_t>(entry);
  }
  LOG_DBG("LIB", "group map: %u groups, %u bytes", static_cast<unsigned>(groupCount),
          static_cast<unsigned>(groupCapacity * sizeof(uint16_t)));
  return groupCount > 0;
}

int LibraryListActivity::groupForBook(const int bookEntry) const {
  int group = 0;
  while (group + 1 < groupCount && groupStarts[group + 1] <= bookEntry) group++;
  return group;
}

bool LibraryListActivity::collapseGroups(const int bookEntry) {
  if (!groupable() || !buildGroupStarts()) return false;
  expandedNav = activeNav();
  groupsCollapsed = true;
  auto& nav = activeNav();
  nav.reset(groupForBook(bookEntry) + 1);
  requestUpdate();
  return true;
}

void LibraryListActivity::expandGroup(const int groupEntry) {
  if (!groupsCollapsed || groupEntry < 0 || groupEntry >= groupCount) return;
  const int bookEntry = groupStarts[groupEntry];
  groupsCollapsed = false;
  activeNav() = expandedNav;
  auto& nav = activeNav();
  nav.selected = bookEntry + 1;
  nav.top = bookEntry;
  nav.followOnBuild = true;
  requestUpdate();
}

void LibraryListActivity::restoreExpandedList() {
  if (!groupsCollapsed) return;
  groupsCollapsed = false;
  activeNav() = expandedNav;
  requestUpdate();
}

// One pass over the sort order, keeping what matches. No index, no cache: at the
// 4096-book format cap this is 4096 comparisons of at most 96 bytes. The result
// array is allocated once with the exact upper bound and fails back to an
// explicit message rather than letting vector growth abort the firmware.
void LibraryListActivity::applyFilter() {
  groupsCollapsed = false;
  groupCount = 0;
  filtered.reset();
  filteredCount = 0;
  filterFailed = false;
  // The header shows the active query in place of the screen title, so the
  // reader can see what narrowed the list without reopening the keyboard.
  headerSearchTitle = query.empty() ? std::string() : "“" + query + "”";
  if (query.empty()) return;

  const std::string needle = library::fold(query);
  const int total = static_cast<int>(index.bookCount());
  if (total <= 0) return;

  auto matches = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(total));
  if (!matches) {
    LOG_ERR("LIB", "cannot allocate %u-byte search result buffer", static_cast<unsigned>(total * sizeof(uint16_t)));
    filterFailed = true;
    return;
  }

  uint16_t matchCount = 0;
  std::string author;
  for (int row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    if (library::matchesQuery(std::string_view(record.fold, record.foldLen), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
      continue;
    }
    // The stored fold covers the title only, so the author has to be read and
    // folded here. That is the search most worth having: the reader who knows
    // the author usually also knows where the book is, while "emily" finding
    // Alice Hunter is the case the shelf exists to answer.
    author.clear();
    if (index.readAuthor(record, author) && library::matchesQuery(library::fold(author), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
    }
  }
  filtered = std::move(matches);
  filteredCount = matchCount;
}

// Staged back-out, shared by the Back button and the header's back arrow:
// clear the search, expand collapsed groups, return focus to the tabs, then
// leave for home.
void LibraryListActivity::handleBackAction() {
  auto& nav = activeNav();
  if (!query.empty()) {
    query.clear();
    applyFilter();
    nav.selected = 0;
    nav.top = 0;
    requestUpdate();
  } else if (groupsCollapsed) {
    restoreExpandedList();
  } else if (!tabsFocused() && !degraded) {
    // Keep the current list and viewport while returning focus to the tabs.
    nav.selected = 0;
    requestUpdate();
  } else {
    cancelBuildAsync();
    onGoHome();
  }
}

void LibraryListActivity::searchActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openSearch();
}

void LibraryListActivity::backActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->handleBackAction();
}

void LibraryListActivity::rebuildActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->queueRebuild();
}

// Title and author for one entry, read straight from the index. Only ever
// called for rows about to be drawn, so at most a screenful of strings exists
// at once.
bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author, std::string* fileName) {
  title.clear();
  author.clear();
  if (fileName) fileName->clear();
  if (entry < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry < 0 || entry >= static_cast<int>(books.size())) return false;
    const auto& book = books[static_cast<size_t>(entry)];
    title = book.title;
    author = book.author;
    if (fileName) *fileName = book.path;
    return true;
  }
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record)) {
    // The build already decided both fields — from the book's own metadata when
    // it has any, and with one spelling chosen per author across the library.
    // Re-parsing the name here would throw that away, and only works while the
    // name still looks like "Title - Author".
    if (!index.readAuthor(record, author)) author.clear();
    // The stored title when the book gave one, the filename otherwise.
    if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
    if (fileName) index.readName(record, *fileName);
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

bool LibraryListActivity::handleCustomInput() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return true;
  }

  return false;
}

bool LibraryListActivity::handleButtons() {
  const int count = listCount();

  // Every hold action fires at the threshold, mid-hold, including the ones
  // that open a dialog (remove-recent, delete). The release that follows is
  // armed as suppressed by wasLongPressed() and consumed globally by
  // ActivityManager::loop() before any activity runs, so it cannot land in
  // the freshly opened confirmation and select its default.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    if (tabsFocused()) {
      if (!degraded) toggleSortDirection();
    } else if (isRecentSort(sortOrder)) {
      showRecentBookOptions(selectedEntry());
    } else if (deleteEligible()) {
      if (count > 0) promptDeleteBook(selectedEntry());
    } else if (!groupsCollapsed && groupable()) {
      collapseGroups(selectedEntry());
    } else {
      activateIndex(selectedEntry());
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBackAction();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (tabsFocused()) {
      stepTab(1);
      return true;
    }
    if (count > 0) activateIndex(selectedEntry());
    return true;
  }

  return false;
}

void LibraryListActivity::navigateButtons() {
  const int count = listCount();
  auto& nav = activeNav();
  buttonNavigator.onNextRelease([this, count] {
    if (count > 0) moveRingTo(ringPos() == count ? 1 : ringPos() + 1);
  });
  buttonNavigator.onPreviousRelease([this, count] {
    if (tabsFocused() && !degraded) {
      openSearch();
    } else if (count > 0) {
      moveRingTo(ringPos() <= 1 ? count : ringPos() - 1);
    }
  });
  // A held button steps tabs while the strip has focus (the base behaviour
  // Settings keeps) and page-jumps once the selection is down in the rows,
  // where fast travel through a long shelf is what a hold means.
  buttonNavigator.onNextContinuous([this, count, &nav] {
    if (tabsFocused()) {
      stepTab(1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::nextPageIndex(selectedEntry(), count, nav.pageRows()) + 1);
    }
  });
  buttonNavigator.onPreviousContinuous([this, count, &nav] {
    if (tabsFocused()) {
      stepTab(-1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::previousPageIndex(selectedEntry(), count, nav.pageRows()) + 1);
    }
  });
}

void LibraryListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = listCount();
  const bool authorGrouped = isAuthorSort(sortOrder);
  const bool grouped = !isRecentSort(sortOrder);

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  props.subtitleText = screen.theme().smallText;
  props.headerText = screen.theme().smallText;
  props.headerUnderline = false;
  syncTabListViewport(screen, props);

  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1);
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (!groupsCollapsed && winHeaders.size() < cap) winHeaders.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);

  int rows = 0;
  int headers = 0;
  uint32_t previousInitial = 0;
  std::string rowFile;
  rowFile.reserve(128);
  // Capture this after syncTabListViewport(), which may clamp nav.top.
  const int windowStart = static_cast<int>(props.topIndex);
  for (int entry = windowStart; entry < count && rows < static_cast<int>(cap); entry++) {
    std::string& title = winTitles[static_cast<size_t>(rows)];
    std::string& author = winAuthors[static_cast<size_t>(rows)];
    fui::ListItem item;
    if (groupsCollapsed) {
      const int bookEntry = groupStarts[entry];
      if (authorGrouped) {
        rowTextFor(bookEntry, title, author);
        formatAuthorHeading(author, title);
      } else {
        formatInitialHeading(titleInitialFor(bookEntry), title);
      }
    } else {
      if (!rowTextFor(entry, title, author, &rowFile)) continue;
      uint32_t initial = 0;
      bool startsGroup = false;
      if (authorGrouped) {
        startsGroup = rows == 0 || author != winAuthors[static_cast<size_t>(rows - 1)];
      } else if (grouped) {
        initial = titleInitialFor(entry);
        startsGroup = rows == 0 || initial != previousInitial;
        previousInitial = initial;
      }
      if (startsGroup) {
        std::string& heading = winHeaders[static_cast<size_t>(headers++)];
        if (authorGrouped)
          formatAuthorHeading(author, heading);
        else
          formatInitialHeading(initial, heading);
        item.sectionHeading = heading.c_str();
      }
      if (!authorGrouped && !author.empty()) item.subtitle = author.c_str();
    }

    item.label = title.c_str();
    // Group headings stay bare; every book row gets its file-type icon.
    if (!groupsCollapsed && !rowFile.empty()) item.icon = listIconFor(UITheme::getFileIcon(rowFile), 32);
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
    rows++;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
  screen.list(props);
  const int next = nav.drawnRows;
  const auto body = screen.body();
  LOG_DBG("LIB", "page tab=%d top=%d full=%d loaded=%d body=%d..%d next=%d title=%s", activeTabIndex, windowStart,
          nav.drawnRows, rows, body.y, body.bottom(),
          next < rows ? winItems[static_cast<size_t>(next)].actionValue : -1,
          next < rows ? winItems[static_cast<size_t>(next)].label : "<none>");
  LOG_DBG("LIB", "page first=%d title=%s", rows > 0 ? winItems[0].actionValue : -1,
          rows > 0 ? winItems[0].label : "<none>");
}

void LibraryListActivity::formatInitialHeading(uint32_t initial, std::string& out) {
  out.clear();
  if (initial == 0) {
    out.push_back('#');
    return;
  }
  if (initial >= 'a' && initial <= 'z') initial -= 'a' - 'A';
  utf8AppendCodepoint(initial, out);
}

void LibraryListActivity::formatAuthorHeading(const std::string& author, std::string& out) const {
  out = author.empty() ? std::string(tr(STR_LIBRARY_UNKNOWN_AUTHOR)) : author;
  if (author.empty()) return;
  const size_t lastSpace = out.find_last_of(' ');
  if (lastSpace != std::string::npos && lastSpace + 1 < out.size()) {
    out = out.substr(lastSpace + 1) + ", " + out.substr(0, lastSpace);
  }
}

void LibraryListActivity::buildHeader(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  fui::HeaderProps header;
  header.title = headerTitle();
  uiTarget.setFont(fui::GfxRendererTarget::FONT_TITLE, GUI.titleFontId(renderer, header.title));
  header.titleText = theme.titleText;
  header.titleText.align = theme.headerTitleAlign;
  header.sidePadding = theme.headerSidePadding;
  header.minTouchSize = theme.minTouchSize;
  header.styles = theme.popup;
  header.styles.normal.radius = 0;
  header.styles.normal.border = fui::Paint::solid(fui::Color::Black);
  header.styles.normal.borderWidth = theme.headerUnderline;
  header.leadingStyles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
  header.trailingStyles = header.leadingStyles;
  header.leadingRadius = 0;
  header.trailingRadius = 0;
  header.borderEdges = fui::EdgeBottom;
  // Same battery/clock band as every GUI.drawHeader screen; the header
  // heights are unified across themes, so the buttons derive from the band.
  GUI.applyHeaderStatus(renderer, header);
  if (mappedInput.hasTouch()) {
    header.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
    header.leadingAction = ACTION_BACK;
    if (!UITheme::isRtl()) header.leftReserve = 14;
  }
  if (!degraded) {
    // Keep both touch actions together on the right; button boards reach
    // rebuild through the row options menu.
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    if (mappedInput.hasTouch()) {
      header.trailingAdjacentIcon = fui::bitmapFromIcon(icon_refresh_cw_32);
      header.trailingAdjacentAction = ACTION_REBUILD;
    }
    // Vertical placement comes from applyHeaderStatus: buttons center on the
    // unified band.
  }
  const auto frameRect = screen.frame().safeRect();
  fui::header(screen.frame(),
              fui::Rect{frameRect.x, static_cast<int16_t>(frameRect.y + metrics.topPadding), frameRect.width,
                        static_cast<int16_t>(metrics.headerHeight)},
              header);
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The position readout owns the line above the hints; rows must not overlap
  // it.
  const int16_t readoutReserved = static_cast<int16_t>(renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing);
  buildHeader(screen);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                     static_cast<int16_t>(metrics.buttonHintsHeight + readoutReserved), 0});

  if (!degraded) buildTabBar(screen);
  else screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (bookRowCount() == 0) {
    char status[96];
    const char* message = tr(STR_LIBRARY_NO_RESULTS);
    if (!index.isOpen()) {
      // No index to browse yet: show the build's progress, and the failure text
      // only once it has stopped.
      message = rebuildStatusText(status, sizeof(status)) ? status : tr(STR_LIBRARY_REBUILD_FAILED);
    } else if (filterFailed) {
      message = tr(STR_LIBRARY_SEARCH_UNAVAILABLE);
    } else if (query.empty()) {
      message = tr(STR_LIBRARY_EMPTY);
    }
    buildListMessage(screen, message);
    return;
  }
  buildRows(screen);
}

// "12/69 books" at the bottom right: which book is selected, out of how many.
//
// NOT a page count. How many rows fit varies with the view (author headings
// consume band height), so a page total grows and shrinks as you scroll. The
// book position is stable by construction, and it answers the question the
// reader actually has: how far in am I, and how much is left.
void LibraryListActivity::drawPositionReadout() const {
  const int count = listCount();
  if (count <= 0) return;

  char buf[96];
  const char* positionFormat = groupsCollapsed ? tr(STR_LIBRARY_GROUP_POSITION) : tr(STR_LIBRARY_POSITION);
  snprintf(buf, sizeof(buf), positionFormat, selectedEntry() + 1, count);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safe = uiTarget.deviceContext().safeArea;
  const int left = safe.left + metrics.contentSidePadding;
  const int right = renderer.getScreenWidth() - safe.right - metrics.contentSidePadding;
  const int width = mappedInput.hasTouch() ? right - left : (right - left - metrics.verticalSpacing) / 2;
  const int x = UITheme::isRtl() ? left : right - width;
  const int height = renderer.getLineHeight(SMALL_FONT_ID);
  const int y = renderer.getScreenHeight() - safe.bottom - metrics.buttonHintsHeight - height;
  fui::TextStyle text;
  text.font = fui::GfxRendererTarget::FONT_LABEL;
  text.align = UITheme::isRtl() ? fui::TextAlign::Left : fui::TextAlign::Right;
  fui::layoutText(uiTarget,
                  fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y),
                            static_cast<int16_t>(width), static_cast<int16_t>(height)},
                  buf, text, [&](const char* line, const fui::Rect run) {
                    renderer.drawText(SMALL_FONT_ID, run.x, run.y, line);
                  });
}

const char* LibraryListActivity::headerTitle() const {
  if (!headerSearchTitle.empty()) return headerSearchTitle.c_str();
  return degraded ? tr(STR_LIBRARY_TITLE_UNSORTED) : tr(STR_LIBRARY);
}

void LibraryListActivity::drawHoldHelp() const {
  // A queued or running refresh owns this line, even on touch boards that
  // otherwise show no help; without an index the body shows it instead.
  char status[96];
  const char* help = nullptr;
  if (index.isOpen() && rebuildStatusText(status, sizeof(status)))
    help = status;
  else if (mappedInput.hasTouch() || groupsCollapsed)
    return;
  else if (tabsFocused() && !degraded)
    help = tr(STR_LIBRARY_HOLD_SORT);
  else if (!tabsFocused() && isRecentSort(sortOrder) && listCount() > 0)
    help = tr(STR_LIBRARY_HOLD_OPTIONS);  // recent rows: hold opens the row menu
  else if (!tabsFocused() && deleteEligible() && listCount() > 0)
    help = tr(STR_HOLD_OPEN_TO_DELETE);
  else if (!tabsFocused() && groupable())
    help = tr(STR_LIBRARY_HOLD_GROUPS);
  if (!help) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const auto safe = uiTarget.deviceContext().safeArea;
  const int left = safe.left + metrics.contentSidePadding;
  const int right = renderer.getScreenWidth() - safe.right - metrics.contentSidePadding;
  const int width = (right - left - metrics.verticalSpacing) / 2;
  const int x = UITheme::isRtl() ? right - width : left;
  const int y = renderer.getScreenHeight() - safe.bottom - metrics.buttonHintsHeight - lineHeight;
  GUI.drawHelpText(renderer, Rect{x, y, width, lineHeight}, help);
}

// OptionPopup is a self-contained modal: it owns the whole frame (hints
// included) whenever it is up, mirroring the FileBrowser pattern.
void LibraryListActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiTabListActivity::render(std::move(lock));
  // The first completed paint releases the queued background refresh.
  firstPainted.store(true, std::memory_order_release);
}

void LibraryListActivity::loop() {
  pollBuild();
  if (!firstPainted.load(std::memory_order_acquire)) return;
  UiTabListActivity::loop();
}

void LibraryListActivity::drawFooter() {
  drawPositionReadout();
  drawHoldHelp();

  const bool backGoesHome = tabsFocused() && !groupsCollapsed && query.empty();
  const char* backLabel = backGoesHome ? tr(STR_HOME) : tr(STR_BACK);
  const char* confirmLabel = groupsCollapsed ? tr(STR_SELECT) : tr(STR_OPEN);
  const bool canSearch = tabsFocused() && !degraded;
  const auto labels = mappedInput.mapLabels(backLabel, tabsFocused() ? tr(STR_TOGGLE) : confirmLabel,
                                            canSearch ? tr(STR_SEARCH) : tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
