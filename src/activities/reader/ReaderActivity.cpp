#include "ReaderActivity.h"

#include <Benchmark.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Memory.h>
#include <RefreshPolicyConfig.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderActivity.h"
#include "Fb2ReaderActivity.h"
#include "HtmlReaderActivity.h"
#include "MarkdownReaderActivity.h"
#include "ReaderUtils.h"
#include "ReadingStats.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "TxtReaderActivity.h"
#include "XtcReaderActivity.h"
#include "components/HeaderBackTapTarget.h"

ReaderActivity::ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::string bookPath, const bool allowFastInitialRefresh)
    : Activity(name, renderer, mappedInput),
      bookPath(std::move(bookPath)),
      refreshPolicy(/*initialClearRequired=*/!allowFastInitialRefresh, refresh_policy_config::active().config) {}

std::unique_ptr<ReaderActivity> ReaderActivity::create(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       std::string path, const bool allowFastInitialRefresh) {
  // ActivityManager requires heap ownership; each branch allocates exactly one screen-lifetime object.
  std::unique_ptr<ReaderActivity> activity;
  if (FsHelpers::hasXtcExtension(path)) {
    activity = makeUniqueNoThrow<XtcReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  } else if (FsHelpers::hasFb2Extension(path)) {
    activity = makeUniqueNoThrow<Fb2ReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  } else if (FsHelpers::hasHtmlExtension(path)) {
    activity = makeUniqueNoThrow<HtmlReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  } else if (FsHelpers::hasMarkdownExtension(path)) {
    activity = makeUniqueNoThrow<MarkdownReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  } else if (FsHelpers::hasTxtExtension(path)) {
    activity = makeUniqueNoThrow<TxtReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  } else {
    activity = makeUniqueNoThrow<EpubReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  }

  if (!activity) {
    LOG_ERR("READER", "OOM: reader activity");
  }
  return activity;
}

void ReaderActivity::applyInitialOrientation() { ReaderUtils::applyOrientation(renderer, SETTINGS.orientation); }

void ReaderActivity::disableFastInitialRefresh() { refreshPolicy.requestClear(); }

void ReaderActivity::onEnter() {
  Activity::onEnter();
  if (isBenchmarkActivity()) benchmarkWaitStart = millis();

  // Heap ledger for field crash reports: free vs largest block distinguishes a
  // leak (free falls) from fragmentation (free stable, largest collapses).
  LOG_INF("MEM", "reader enter: free=%u max_block=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  if (!Storage.exists(bookPath.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", bookPath.c_str());
    if (isBenchmarkActivity()) finishBenchmark(BenchmarkOutcome::LoadFailed);
    else finish();
    return;
  }

  // Clear remembered book after opening it
  if (!isBenchmarkActivity() && !APP_STATE.openEpubPath.empty()) {
    APP_STATE.openEpubPath.clear();
    APP_STATE.saveToFile();
  }

  // Boot and font-settings screens already load the selected fonts. A private
  // run must not invoke font-repair code that can persist fallback settings.
  if (!isBenchmarkActivity()) sdFontSystem.ensureLoaded(renderer);
  applyInitialOrientation();

  bool loaded = false;
  {
    // Benchmark loading also paints indexing popups on the shared framebuffer.
    RenderLock lock;
    loaded = loadBook();
  }
  if (!loaded) {
    if (isBenchmarkActivity()) finishBenchmark(BenchmarkOutcome::LoadFailed);
    else finish();
    return;
  }

  requestUpdate();
}

void ReaderActivity::rememberBookOnceRendered(const bool foreground) {
  if (!isBenchmarkActivity()) {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock()) {
      drainReadingStats(foreground);
      if (!foreground) reading_stats::suspend();
    }
  }
  if (isBenchmarkActivity() || bookRemembered || !pageRendered.load(std::memory_order_acquire)) return;
  bookRemembered = true;
  APP_STATE.openEpubPath = bookPath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(bookPath, getBookTitle(), getBookAuthor(), getBookThumbBmpPath());
}

void ReaderActivity::onExit() {
  if (!isBenchmarkActivity() && statsRendered) {
    drainReadingStats(false);
    reading_stats::finish();
  }
  if (isBenchmarkActivity()) {
    // A cancellation can arrive while the render task is still painting. Exit
    // owns RenderLock, so include that last completed turn without counting it twice.
    if (benchmarkWaiting && !benchmarkInitialPage && !benchmarkRenderFailed &&
        benchmarkGeneration != benchmarkSeenGeneration &&
        (benchmarkChapter > benchmarkPreviousChapter ||
         (benchmarkChapter == benchmarkPreviousChapter && benchmarkPage > benchmarkPreviousPage))) {
      ++*benchmarkCompleted;
    }
    const auto* child = std::get_if<BenchmarkResult>(&result.data);
    const auto outcome = child ? child->outcome : BenchmarkOutcome::Cancelled;
    benchmark::point(benchmark::EventType::RunStep, benchmarkPhase, *benchmarkCompleted, benchmarkTarget,
                     static_cast<uint32_t>(outcome));
  }
  Activity::onExit();

  // Keep rebuildable font buffers from pinning the heap between reading sessions.
  if (auto* fontCache = renderer.getFontCacheManager()) {
    fontCache->releaseSdFontCaches();
  }

  LOG_INF("MEM", "reader exit: free=%u max_block=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  ReaderUtils::applyMenuOrientation(renderer);
  if (!isBenchmarkActivity()) {
    APP_STATE.readerActivityLoadCount = 0;
    APP_STATE.saveToFile();
  }

  endOfBookOptions.reset();
  endOfBookOptionsReady.store(false, std::memory_order_release);
}

bool ReaderActivity::handleBackNavigation() {
  return ReaderUtils::handleBackNavigation(mappedInput, activityManager, bookPath.c_str(),
                                           {this, [](void* ctx) { static_cast<ReaderActivity*>(ctx)->onGoHome(); }});
}

void ReaderActivity::clearEndOfBookOptionsIfNeeded() {
  if (isAtEndOfBook() || !endOfBookOptionsReady.load(std::memory_order_acquire)) return;

  RenderLock lock(*this);
  endOfBookOptionsReady.store(false, std::memory_order_release);
  endOfBookOptions.reset();
}

bool ReaderActivity::endOfBookMenuActive() const {
  return isAtEndOfBook() && endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive();
}

bool ReaderActivity::handleEndOfBookMenu(const bool suppressConfirmRelease) {
  if (suppressConfirmRelease || !endOfBookMenuActive()) {
    return false;
  }

  std::string openPath;
  switch (endOfBookOptions->handleMenuInput(mappedInput, &openPath)) {
    case EndOfBookOptions::Action::OpenBook:
      activityManager.goToReader(openPath);
      return true;
    case EndOfBookOptions::Action::GoHome:
      onGoHome();
      return true;
    case EndOfBookOptions::Action::LastPage:
      onReturnFromEndOfBook();
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::Redraw:
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::None:
      return false;
  }

  return false;
}

bool ReaderActivity::handleEndOfBookPageTurn(const bool prevTriggered, const bool nextTriggered) {
  if (!isAtEndOfBook()) return false;

  if (endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->hasSuggestions()) {
    return true;
  }
  if (nextTriggered) {
    onGoHome();
  } else if (prevTriggered) {
    onReturnFromEndOfBook();
    requestUpdate();
  }
  return true;
}

void ReaderActivity::configureBenchmark(const uint8_t phase, const uint8_t turns, uint8_t& completedTurns) {
  benchmarkPhase = phase;
  benchmarkTarget = turns;
  benchmarkCompleted = &completedTurns;
  completedTurns = 0;
}

void ReaderActivity::markPageRendered(const uint32_t chapter, const uint32_t page, const uint32_t progress) {
  if (!isBenchmarkActivity()) {
    if (statsRendered && (statsChapter != chapter || statsPage != page)) {
      ++statsPendingPages;
      if (progress > statsProgress) statsPendingProgress += progress - statsProgress;
    }
    statsRendered = true;
    statsChapter = chapter;
    statsPage = page;
    statsProgress = std::min<uint32_t>(progress, 10000U);
  }
  pageRendered.store(true, std::memory_order_release);
  // Called with RenderLock held; normal reading needs no generation bookkeeping.
  if (!isBenchmarkActivity()) return;
  benchmarkChapter = chapter;
  benchmarkPage = page;
  ++benchmarkGeneration;
}

void ReaderActivity::onCovered() {
  if (isBenchmarkActivity()) return;
  drainReadingStats(false);
  reading_stats::suspend();
}

void ReaderActivity::drainReadingStats(const bool foreground) {
  if (!statsRendered) return;
  if (!statsStarted) {
    reading_stats::begin(bookPath.c_str(), getBookTitle().c_str(), statsProgress);
    statsStarted = true;
  }
  if (foreground) {
    int x, y;
    const bool input = mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased() ||
                       mappedInput.wasScreenTouchDown(x, y) || mappedInput.wasScreenTouchReleased();
    reading_stats::tick(input);
  }
  auto& stats = reading_stats::data();
  if (statsPendingPages) {
    // Consume every completed navigation, including a final frame before sleep.
    const uint32_t advanced = stats.book.advanced + statsPendingProgress;
    while (statsPendingPages) {
      reading_stats::page(statsProgress);
      --statsPendingPages;
    }
    stats.book.advanced = advanced;
    statsPendingProgress = 0;
  }
}

void ReaderActivity::markBenchmarkRenderFailed() {
  if (isBenchmarkActivity()) benchmarkRenderFailed = true;
}

bool ReaderActivity::benchmarkCancelled() const {
  return mappedInput.wasReleased(MappedInputManager::Button::Back) ||
         mappedInput.wasReleased(MappedInputManager::Button::Power) || mappedInput.wasBackGesture() ||
         mappedInput.wasHomeGesture() || mappedInput.homeButtonAction() != HomeButtonAction::Ignore ||
         mappedInput.wasLightPanelGesture();
}

void ReaderActivity::finishBenchmark(const BenchmarkOutcome outcome) {
  if (benchmarkFinished) return;
  benchmarkFinished = true;
  setResult(BenchmarkResult{outcome});
  finish();
}

bool ReaderActivity::benchmarkLoop() {
  if (!isBenchmarkActivity()) return false;
  if (benchmarkFinished) return true;
  if (benchmarkCancelled()) {
    finishBenchmark(BenchmarkOutcome::Cancelled);
    return true;
  }
  // Pause on press, finish on release so the release cannot activate Run again
  // in the restored menu. The main loop still owns manual power-hold sleep.
  if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
      mappedInput.isPressed(MappedInputManager::Button::Power)) return true;
  if (!benchmark::enabled()) {
    finishBenchmark(BenchmarkOutcome::RecorderFailed);
    return true;
  }
  const uint32_t now = millis();
  if (benchmarkWaiting && now - benchmarkWaitStart >= 120000) {
    finishBenchmark(BenchmarkOutcome::Timeout);
    return true;
  }
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return true;
  if (benchmarkRenderFailed) {
    finishBenchmark(BenchmarkOutcome::RenderFailed);
    return true;
  }
  if (isAtEndOfBook()) {
    finishBenchmark(BenchmarkOutcome::NoProgress);
    return true;
  }
  if (benchmarkWaiting) {
    if (benchmarkGeneration == benchmarkSeenGeneration) return true;
    benchmarkSeenGeneration = benchmarkGeneration;
    if (!benchmarkInitialPage) {
      if (benchmarkChapter < benchmarkPreviousChapter ||
          (benchmarkChapter == benchmarkPreviousChapter && benchmarkPage <= benchmarkPreviousPage)) {
        finishBenchmark(BenchmarkOutcome::NoProgress);
        return true;
      }
      ++*benchmarkCompleted;
    }
    benchmarkInitialPage = false;
    benchmarkPreviousChapter = benchmarkChapter;
    benchmarkPreviousPage = benchmarkPage;
    benchmarkWaiting = false;
    benchmarkDwellStart = now;
    benchmark::point(benchmark::EventType::RunStep, benchmarkPhase, *benchmarkCompleted, benchmarkTarget, 8);
    return true;
  }
  if (now - benchmarkDwellStart < 2000) return true;
  if (*benchmarkCompleted >= benchmarkTarget) {
    finishBenchmark(BenchmarkOutcome::Complete);
    return true;
  }
  benchmarkWaiting = true;
  benchmarkWaitStart = now;
  benchmarkSeenGeneration = benchmarkGeneration;
  // pageTurn owns its own RenderLock. Never nest the non-recursive mutex.
  lock.unlock();
  if (!pageTurn(true)) {
    finishBenchmark(BenchmarkOutcome::NoProgress);
    return true;
  }
  requestUpdate();
  return true;
}

void ReaderActivity::loop() {
  if (benchmarkLoop()) return;
  rememberBookOnceRendered();
  clearEndOfBookOptionsIfNeeded();
  if (handleEndOfBookMenu()) return;
  if (handleFormatInput()) return;
  if (handleBackNavigation()) return;

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) return;
  if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) return;

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool skip =
      !fromTilt && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP && heldMs >= ReaderUtils::SKIP_HOLD_MS;

  if (prevTriggered) {
    if (skip) {
      skipPages(-10);
    } else {
      pageTurn(false);
    }
  } else {
    if (skip) {
      skipPages(10);
    } else {
      pageTurn(true);
    }
  }
  requestUpdate();
}

void ReaderActivity::render(RenderLock&&) {
  HeaderBackTapTarget::clear();
  if (isAtEndOfBook()) {
    if (isBenchmarkActivity()) return;
    if (!endOfBookOptions) {
      endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);
      if (!endOfBookOptions) LOG_ERR("READER", "OOM: EndOfBookOptions");
    }
    renderer.clearScreen();
    if (endOfBookOptions) {
      endOfBookOptions->loadOnce(bookPath);
      // Release-publish AFTER loadOnce() so the main task's acquire load can't
      // observe an object whose names/selector are still being populated.
      endOfBookOptionsReady.store(true, std::memory_order_release);
      endOfBookOptions->render(renderer, mappedInput);
    }
    renderer.displayBuffer();
    onEndOfBookRendered();
    if (endOfBookOptions) markPageRendered();
    return;
  }

  renderBook();
}

bool ReaderActivity::handleForcedRefresh() {
  if (isBenchmarkActivity()) return true;
  {
    RenderLock lock(*this);
    refreshPolicy.requestClear();
  }
  requestUpdate();
  return true;
}
