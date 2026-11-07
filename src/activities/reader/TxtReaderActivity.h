#pragma once

#include <Txt.h>
#include <TxtBackgroundIndexing.h>
#include <TxtPageIndexFormat.h>
#include <TxtPagination.h>

#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "ReaderActivity.h"

class ReaderWordPage;

class TxtReaderActivity final : public ReaderActivity {
 public:
  // One display line of the current page and its first-line indent in pixels.
  struct DisplayLine {
    std::string text;
    int indent = 0;
  };

 private:
  std::unique_ptr<Txt> txt;

  // The page index, extended a page per loop pass once the first page is up.
  // Pages before its frontier are final; a position past the frontier is
  // shown by laying out from its offset, and gets its page number once the
  // index reaches it.
  TxtPagination::PageIndexBuilder pageIndex;
  bool indexFailed = false;
  // Pages laid out since the index was last written: a partial index is
  // checkpointed now and then and on exit, so a reopen resumes it.
  size_t pagesSinceSave = 0;
  TxtBackgroundIndexing::QuietGate indexingQuietGate;

  // Source offset of the displayed page. Usually an index page start; after a
  // resume or jump past the frontier, or into a new layout, any line start.
  size_t currentOffset = 0;
  bool atEndOfBook = false;
  // The page last rendered and where the page after it starts.
  size_t displayedOffset = static_cast<size_t>(-1);
  size_t displayedNextOffset = static_cast<size_t>(-1);

  std::vector<DisplayLine> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  // Chinese paragraph style (see TxtPagination::PageLayout), chosen per book.
  bool cjkParagraphs = false;
  int firstLineIndent = 0;
  bool initialized = false;
  // "No dictionary selected" note after a hold or the menu's Dictionary entry.
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;

  // Retained source handle and 8KB page window, shared by indexing and page renders.
  TxtPagination::SourceWindow sourceWindow;

  // Direct-mapped per-code-point advances of the reader font, for line-end estimates.
  struct AdvanceCacheEntry {
    uint32_t codepoint;
    int32_t advance;
  };
  static constexpr size_t ADVANCE_CACHE_SIZE = 512;
  std::unique_ptr<AdvanceCacheEntry[]> advanceCache;

  // Cached settings for cache validation
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage(GfxRenderer& renderer);
  void initializeReader(GfxRenderer& renderer);
  // Screen x of a display line under the current alignment and RTL rule.
  int lineX(GfxRenderer& renderer, const DisplayLine& line) const;
  // A snapshot of the displayed lines and their positions, for the dictionary
  // picker and hold lookup. Requires the render lock.
  std::unique_ptr<ReaderWordPage> makeWordPage(GfxRenderer& renderer);
  // Opens the picker on the current page; false when there is no page yet.
  bool openDictionaryWordSelect();
  // Press and hold on a word: look it up directly. True when the hold was
  // taken (lookup or "no dictionary" note).
  bool handleWordHold();
  // Lays out the page at `offset` into `outLines`.
  bool loadPageAtOffset(const GfxRenderer& renderer, size_t offset, std::vector<DisplayLine>& outLines,
                        size_t& nextOffset);
  // Start of the page after the one at `offset`.
  bool nextPageOffset(size_t offset, size_t& next);

  // Extends the index by at most `maxPages`; false once indexing has failed.
  bool extendIndex(size_t maxPages, bool background = false);
  // Extends the index behind an "Indexing" popup until it covers `offset`, or
  // holds `page` when `page` >= 0; false when indexing failed.
  bool indexThrough(size_t offset, int page);
  void indexInBackground();
  bool shouldYieldBackgroundIndexing();

  bool stepForward();
  bool stepBackward();
  void showPage(int page);
  // Shows the index page holding `offset`, or past the frontier `offset` itself.
  void showOffset(size_t offset);
  // Start of the first non-blank line after `offset` within one page window, or
  // the code point at `offset` when there is none.
  size_t paragraphStartFrom(size_t offset);
  void jumpToPercent(int percent);

  bool loadPageIndexCache();
  // The layout and source a stored index must have been built for to be reused.
  TxtPageIndexFormat::Key pageIndexKey() const;
  void savePageIndexCache();
  // Offset of `page` in the index earlier firmware wrote, to carry its
  // page-only reading position over to the current layout.
  bool legacyPageOffset(uint32_t page, uint32_t& offset) const;
  void saveProgress() const;
  void loadProgress();

  void renderStatusBar() const;
  size_t estimatedBytesPerPage() const;
  int displayedPageNumber() const;
  // Exact once indexing is complete, an estimate before.
  int displayedPageCount() const;
  float bookProgressPercent() const;
  int indexPercent() const;

  void openMenu();
  void openContents();
  void openPageSelection();
  void openPercentSelection();

  bool loadBook() override;
  std::string getBookTitle() const override { return txt ? txt->getTitle() : ""; }
  void renderBook() override;
  bool handleFormatInput() override;

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                             bool allowFastInitialRefresh)
      : ReaderActivity("TxtReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~TxtReaderActivity() override = default;

  void onExit() override;
  void loop() override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  // Advance of `cp` in the reader font, through the per-code-point cache.
  int codepointAdvance(const GfxRenderer& renderer, uint32_t cp);

  ScreenshotInfo getScreenshotInfo() const override;
};
