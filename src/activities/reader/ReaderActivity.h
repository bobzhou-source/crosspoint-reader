#pragma once

#include <AdaptiveRefreshPolicy.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "EndOfBookOptions.h"
#include "activities/Activity.h"

struct AnnotationEntry;

class ReaderActivity : public Activity {
 protected:
  std::string bookPath;
  // Chooses fast vs clearing refreshes for this reader's pages; guarded by the render lock.
  AdaptiveRefreshPolicy refreshPolicy;

  std::unique_ptr<EndOfBookOptions> endOfBookOptions;
  std::atomic<bool> endOfBookOptionsReady{false};
  std::atomic<bool> pageRendered{false};
  bool bookRemembered = false;
  void markPageRendered(uint32_t chapter = 0, uint32_t page = 0, uint32_t progress = 10000);
  void drainReadingStats(bool foreground);
  bool statsStarted = false;
  bool statsRendered = false;
  uint32_t statsChapter = 0;
  uint32_t statsPage = 0;
  uint32_t statsProgress = 0;
  uint32_t statsPendingPages = 0;
  uint32_t statsPendingProgress = 0;
  void markBenchmarkRenderFailed();
  bool benchmarkLoop();
  bool benchmarkCancelled() const;
  void finishBenchmark(BenchmarkOutcome outcome);
  void rememberBookOnceRendered(bool foreground = true);

  explicit ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                          std::string bookPath, bool allowFastInitialRefresh);

  virtual bool loadBook() = 0;
  virtual std::string getBookTitle() const = 0;
  virtual std::string getBookAuthor() const { return ""; }
  virtual std::string getBookThumbBmpPath() const { return ""; }

  virtual bool handleFormatInput() { return false; }
  virtual bool pageTurn(bool isForward) = 0;
  virtual bool skipPages(int amount) { return pageTurn(amount > 0); }
  virtual bool isAtEndOfBook() const = 0;
  virtual void onReturnFromEndOfBook() {}

  virtual void renderBook() = 0;
  virtual void applyInitialOrientation();
  virtual void onEndOfBookRendered() {}

  bool handleBackNavigation();
  /** True while the end-of-book suggestion menu is on screen and owning input. */
  bool endOfBookMenuActive() const;
  bool handleEndOfBookMenu(bool suppressConfirmRelease = false);
  bool handleEndOfBookPageTurn(bool prevTriggered, bool nextTriggered);
  void clearEndOfBookOptionsIfNeeded();
  void disableFastInitialRefresh();

 public:
  ~ReaderActivity() override = default;

  static std::unique_ptr<ReaderActivity> create(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                std::string path, bool allowFastInitialRefresh);

  // Called before onEnter to open a native annotation instead of saved progress.
  // EPUB-based readers support this; other formats reject it explicitly.
  virtual bool openAnnotation(const AnnotationEntry&) { return false; }

  void onEnter() override;
  // The coordinator outlives its pushed reader and owns completedTurns.
  void configureBenchmark(uint8_t phase, uint8_t turns, uint8_t& completedTurns);
  bool isBenchmarkActivity() const final { return benchmarkPhase != 0; }
  bool preventAutoSleep() override { return isBenchmarkActivity(); }
  void onExit() override;
  void loop() override;
  void onCovered() override;
  void render(RenderLock&& lock) override;

  // Set only after a normal page (or the end-of-book UI) completed its display
  // update; error and indexing screens leave it false.
  bool hasCompletedHealthyRender() const override { return pageRendered.load(std::memory_order_acquire); }

  bool isReaderActivity() const final { return true; }
  bool handleForcedRefresh() final;

 private:
  uint8_t benchmarkPhase = 0;
  uint8_t benchmarkTarget = 0;
  uint8_t* benchmarkCompleted = nullptr;
  uint32_t benchmarkGeneration = 0;
  uint32_t benchmarkSeenGeneration = 0;
  uint32_t benchmarkChapter = 0;
  uint32_t benchmarkPage = 0;
  uint32_t benchmarkPreviousChapter = 0;
  uint32_t benchmarkPreviousPage = 0;
  uint32_t benchmarkWaitStart = 0;
  uint32_t benchmarkDwellStart = 0;
  bool benchmarkWaiting = true;
  bool benchmarkInitialPage = true;
  bool benchmarkRenderFailed = false;
  bool benchmarkFinished = false;
};
