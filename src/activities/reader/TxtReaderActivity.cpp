#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <TxtLineBreak.h>
#include <TxtPageIndexFormat.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "DictionaryPageWords.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "ProgressFile.h"
#include "ReaderActivity.h"
#include "ReaderTouchZones.h"
#include "ReaderUtils.h"
#include "ReaderWordPage.h"
#include "ReadingPacing.h"
#include "TxtReaderChapterSelectionActivity.h"
#include "TxtReaderMenuActivity.h"
#include "TxtReaderPageSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ConceptEngine.h"
#include "util/DictionaryRegistry.h"

namespace {
// Background work yields for input between font-metric batches and display lines.
constexpr size_t BACKGROUND_PAGES_PER_PASS = 1;
// Pages laid out between yields while the reader waits on the index behind a popup.
constexpr size_t FOREGROUND_PAGES_PER_STEP = 16;
// A partial index is written after this many new pages (and on exit), so a
// reopened book resumes indexing instead of starting over.
constexpr size_t CHECKPOINT_PAGES = 1024;
// The popup progress bar is redrawn (a display refresh) only in steps of this many percent.
constexpr int POPUP_PROGRESS_STEP = 10;
// No next-page offset is known.
constexpr size_t NO_OFFSET = static_cast<size_t>(-1);

struct PageMeasure {
  const GfxRenderer* renderer;
  int fontId;
  TxtReaderActivity* reader;
  bool (*shouldYield)(TxtReaderActivity*) = nullptr;
  bool interrupted = false;
};

int measureLine(void* ctx, const char* text) {
  const auto* measure = static_cast<const PageMeasure*>(ctx);
  return measure->renderer->getTextAdvanceX(measure->fontId, text, EpdFontFamily::REGULAR);
}

int measureCodepoint(void* ctx, const uint32_t cp) {
  const auto* measure = static_cast<const PageMeasure*>(ctx);
  return measure->reader->codepointAdvance(*measure->renderer, cp);
}

bool yieldIndexing(void* ctx) {
  auto* measure = static_cast<PageMeasure*>(ctx);
  if (!measure->interrupted && measure->shouldYield) {
    measure->interrupted = measure->shouldYield(measure->reader);
  }
  return measure->interrupted;
}

void prepareSdFontSpan(void* ctx, const char* pageText) {
  const auto* measure = static_cast<const PageMeasure*>(ctx);
  measure->renderer->ensureSdCardFontReady(measure->fontId, pageText, /*styleMask=*/0x01);
}

void prepareSdFontPage(void* ctx, const char* pageText) {
  auto* measure = static_cast<PageMeasure*>(ctx);
  if (measure->shouldYield) {
    if (!TxtBackgroundIndexing::prepareInChunks(pageText, ctx, prepareSdFontSpan, yieldIndexing)) {
      measure->interrupted = true;
    }
  } else {
    prepareSdFontSpan(ctx, pageText);
  }
}

void appendLine(void* ctx, const char* text, size_t length, const int indent) {
  static_cast<std::vector<TxtReaderActivity::DisplayLine>*>(ctx)->push_back({std::string(text, length), indent});
}

TxtPagination::PageLayout makeLayout(PageMeasure& measure, const int viewportWidth, const int linesPerPage,
                                     const bool cjkParagraphs, const int firstLineIndent) {
  TxtPagination::PageLayout layout{
      viewportWidth,
      linesPerPage,
      {&measure, measure.renderer->isSdCardFont(measure.fontId) ? prepareSdFontPage : nullptr, measureLine,
       measureCodepoint}};
  layout.cjkParagraphs = cjkParagraphs;
  layout.firstLineIndent = firstLineIndent;
  return layout;
}

size_t encodeUtf8(const uint32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

// Exact-length reads through a small buffer; the index reader asks for a few bytes at a time.
class BufferedReader {
 public:
  explicit BufferedReader(HalFile& file) : file(file) {}

  bool operator()(uint8_t* out, size_t length) {
    while (length > 0) {
      if (position == end) {
        const int got = file.read(buffer, sizeof(buffer));
        if (got <= 0) return false;
        position = 0;
        end = static_cast<size_t>(got);
      }
      const size_t count = std::min(length, end - position);
      memcpy(out, buffer + position, count);
      position += count;
      out += count;
      length -= count;
    }
    return true;
  }

 private:
  HalFile& file;
  uint8_t buffer[128];
  size_t position = 0;
  size_t end = 0;
};
}  // namespace

bool TxtReaderActivity::loadBook() {
  txt = makeUniqueNoThrow<Txt>(bookPath, "/.crosspoint");
  if (!txt) {
    LOG_ERR("TRS", "Failed to allocate TXT object");
    return false;
  }
  if (!txt->load()) {
    LOG_ERR("TRS", "Failed to load TXT");
    return false;
  }
  txt->setupCacheDir();
  sourceWindow.setSource(txt->getPath());
  return true;
}

void TxtReaderActivity::onExit() {
  // Sleep and leaving the book both pass here: keep the indexing done so far.
  if (initialized && !pageIndex.complete() && !indexFailed && pagesSinceSave > 0) {
    savePageIndexCache();
  }
  sourceWindow.release();
  ReaderActivity::onExit();
}

void TxtReaderActivity::loop() {
  // A held word opens its definition instead of turning the page on release.
  if (handleWordHold()) return;
  ReaderActivity::loop();
  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }
  recordDwellContext();
  indexInBackground();
}

int TxtReaderActivity::codepointAdvance(const GfxRenderer& renderer, const uint32_t cp) {
  AdvanceCacheEntry* entry = advanceCache ? &advanceCache[cp % ADVANCE_CACHE_SIZE] : nullptr;
  if (entry && entry->codepoint == cp) {
    return entry->advance;
  }
  char encoded[5];
  encoded[encodeUtf8(cp, encoded)] = '\0';
  const int advance = renderer.getTextAdvanceX(cachedFontId, encoded, EpdFontFamily::REGULAR);
  if (entry) {
    *entry = {cp, advance};
  }
  return advance;
}

void TxtReaderActivity::initializeReader(GfxRenderer& renderer) {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.readerMargin(renderer.getScreenWidth() > renderer.getScreenHeight());
  cachedParagraphAlignment = SETTINGS.paragraphAlignment == CrossPointSettings::BOOK_STYLE
                                 ? CrossPointSettings::LEFT_ALIGN
                                 : SETTINGS.paragraphAlignment;

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;
  currentPageLines.reserve(linesPerPage);

  // Line-end estimates read per-code-point advances of this font; a failed allocation only costs speed.
  if (!advanceCache) {
    advanceCache = makeUniqueNoThrow<AdvanceCacheEntry[]>(ADVANCE_CACHE_SIZE);
  }
  if (advanceCache) {
    std::fill(advanceCache.get(), advanceCache.get() + ADVANCE_CACHE_SIZE, AdvanceCacheEntry{UINT32_MAX, 0});
  }

  // Chinese paragraph style follows the book's script, judged from its opening text.
  const size_t fileSize = txt->getFileSize();
  cjkParagraphs = false;
  firstLineIndent = 0;
  char* opening = nullptr;
  const size_t openingLength = TxtPagination::pageInputLength(0, fileSize);
  if (openingLength > 0 &&
      sourceWindow.load(0, openingLength, opening) == TxtPagination::ReadStatus::Ok) {
    cjkParagraphs = TxtLineBreak::isCjkDominant(opening, openingLength);
  }
  if (cjkParagraphs) {
    static constexpr char TWO_EM[] = "\xE4\xB8\xAD\xE4\xB8\xAD";  // 中中
    if (renderer.isSdCardFont(cachedFontId)) {
      renderer.ensureSdCardFontReady(cachedFontId, TWO_EM, /*styleMask=*/0x01);
    }
    firstLineIndent =
        std::clamp(renderer.getTextAdvanceX(cachedFontId, TWO_EM, EpdFontFamily::REGULAR), 0, viewportWidth / 2);
  }

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d, CJK paragraphs: %d", viewportWidth, viewportHeight,
          linesPerPage, cjkParagraphs ? 1 : 0);

  indexFailed = false;
  pagesSinceSave = 0;
  if (!loadPageIndexCache()) {
    pageIndex.reset(fileSize);
  }

  initialized = true;
  loadProgress();
}

bool TxtReaderActivity::extendIndex(const size_t maxPages, const bool background) {
  if (indexFailed) return false;
  if (pageIndex.complete()) return true;

  PageMeasure measure{&renderer, cachedFontId, this};
  auto layout = makeLayout(measure, viewportWidth, linesPerPage, cjkParagraphs, firstLineIndent);
  if (background) {
    measure.shouldYield = [](TxtReaderActivity* reader) { return reader->shouldYieldBackgroundIndexing(); };
    layout.measure.shouldYield = yieldIndexing;
  }
  const size_t before = pageIndex.offsets().size();
  const auto result = pageIndex.step(sourceWindow, layout, maxPages);
  if (result.status == TxtPagination::IndexStatus::Interrupted) return true;
  if (result.status != TxtPagination::IndexStatus::Ok) {
    LOG_ERR("TRS", "Page index build failed at offset %zu (status %d, read %d)", result.offset,
            static_cast<int>(result.status), static_cast<int>(result.read));
    indexFailed = true;
    return false;
  }

  const size_t after = pageIndex.offsets().size();
  pagesSinceSave += after > before ? after - before : 1;
  if (pageIndex.complete()) {
    LOG_DBG("TRS", "Built page index: %zu pages, %zu chapters", after, pageIndex.chapters().size());
    savePageIndexCache();
  } else if (pagesSinceSave >= CHECKPOINT_PAGES) {
    savePageIndexCache();
  }
  return true;
}

bool TxtReaderActivity::indexThrough(const size_t offset, const int page) {
  const auto reached = [&] {
    return pageIndex.complete() ||
           (page >= 0 ? pageIndex.offsets().size() > static_cast<size_t>(page) : pageIndex.covers(offset));
  };
  if (reached()) return true;
  if (indexFailed) return false;

  const Rect popup = GUI.drawPopup(renderer, tr(STR_INDEXING));
  refreshPolicy.requestClear(AdaptiveRefreshPolicy::ClearSource::Popup);

  const size_t target = std::max<size_t>(1, std::min(offset, txt->getFileSize()));
  int shownPercent = 0;
  while (!reached()) {
    if (!extendIndex(FOREGROUND_PAGES_PER_STEP)) {
      markBenchmarkRenderFailed();
      return false;
    }
    vTaskDelay(1);  // let other tasks run between steps

    const int percent =
        page >= 0 ? static_cast<int>(pageIndex.offsets().size() * 100 / (static_cast<size_t>(page) + 1))
                  : static_cast<int>(static_cast<uint64_t>(pageIndex.frontier()) * 100 / target);
    if (percent >= shownPercent + POPUP_PROGRESS_STEP) {
      shownPercent = std::min(100, percent - percent % POPUP_PROGRESS_STEP);
      GUI.fillPopupProgress(renderer, popup, shownPercent);
    }
  }
  return true;
}

bool TxtReaderActivity::shouldYieldBackgroundIndexing() {
  float nx = 0;
  float ny = 0;
  bool pending = gpio.rawInputActive() || gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() ||
                 gpio.isTouchHeldAt(nx, ny);
  if (!pending) {
    using Button = MappedInputManager::Button;
    for (const Button button : {Button::Back, Button::Confirm, Button::Left, Button::Right, Button::Up, Button::Down,
                                Button::Power}) {
      if (mappedInput.isPressed(button)) {
        pending = true;
        break;
      }
    }
  }
  return !indexingQuietGate.ready(millis(), pending);
}

void TxtReaderActivity::indexInBackground() {
  if (pageIndex.complete() || indexFailed || shouldYieldBackgroundIndexing()) return;
  // Never wait on a render: indexing resumes on a later pass.
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock() || !initialized) return;
  extendIndex(BACKGROUND_PAGES_PER_PASS, true);
}

bool TxtReaderActivity::nextPageOffset(const size_t offset, size_t& next) {
  if (displayedOffset == offset && displayedNextOffset != NO_OFFSET) {
    next = displayedNextOffset;
    return true;
  }
  PageMeasure measure{&renderer, cachedFontId, this};
  const auto layout = makeLayout(measure, viewportWidth, linesPerPage, cjkParagraphs, firstLineIndent);
  const auto page = TxtPagination::loadPage(sourceWindow, offset, txt->getFileSize(), layout, nullptr);
  if (page.status != TxtPagination::ReadStatus::Ok) {
    LOG_ERR("TRS", "Failed to lay out page at offset %zu (status %d)", offset, static_cast<int>(page.status));
    return false;
  }
  next = page.nextOffset;
  return true;
}

bool TxtReaderActivity::stepForward() {
  if (atEndOfBook) return false;
  const size_t fileSize = txt->getFileSize();
  const auto& offsets = pageIndex.offsets();
  const size_t page = pageIndex.pageContaining(currentOffset);
  const bool onIndexedPage = pageIndex.covers(currentOffset) && offsets[page] == currentOffset;

  // Off the index (or on its frontier page) the next page starts where this one
  // ends: the same offset the index records, since layout depends only on a
  // page's start.
  size_t next;
  if (onIndexedPage && page + 1 < offsets.size()) {
    next = offsets[page + 1];
  } else if (onIndexedPage && pageIndex.complete()) {
    next = fileSize;
  } else if (!nextPageOffset(currentOffset, next)) {
    return false;
  }

  if (next >= fileSize || next <= currentOffset) {
    atEndOfBook = true;
    return true;
  }
  currentOffset = next;
  return true;
}

bool TxtReaderActivity::stepBackward() {
  if (atEndOfBook) {
    atEndOfBook = false;
    return true;
  }
  // The page before an unindexed position is unknown until the index reaches it.
  if (!pageIndex.covers(currentOffset) && !indexThrough(currentOffset, -1)) {
    return false;
  }
  const auto& offsets = pageIndex.offsets();
  const size_t page = pageIndex.pageContaining(currentOffset);
  if (offsets[page] < currentOffset) {
    currentOffset = offsets[page];
  } else if (page > 0) {
    currentOffset = offsets[page - 1];
  } else {
    return false;
  }
  return true;
}

void TxtReaderActivity::showPage(const int page) {
  const auto& offsets = pageIndex.offsets();
  const size_t clamped = std::min(static_cast<size_t>(std::max(page, 0)), offsets.size() - 1);
  currentOffset = offsets[clamped];
  atEndOfBook = false;
}

void TxtReaderActivity::showOffset(const size_t offset) {
  atEndOfBook = false;
  if (pageIndex.covers(offset)) {
    showPage(static_cast<int>(pageIndex.pageContaining(offset)));
  } else {
    currentOffset = offset;
  }
}

size_t TxtReaderActivity::paragraphStartFrom(const size_t offset) {
  const size_t fileSize = txt->getFileSize();
  if (offset == 0 || offset >= fileSize) {
    return std::min(offset, fileSize);
  }
  // From a few bytes before: a line starting exactly at `offset` counts, and the lead byte of the code point
  // holding `offset` (at most 3 bytes back) is in reach.
  const size_t from = offset > 3 ? offset - 4 : 0;
  const size_t length = TxtPagination::pageInputLength(from, fileSize);
  char* text = nullptr;
  if (sourceWindow.load(from, length, text) != TxtPagination::ReadStatus::Ok) {
    return NO_OFFSET;
  }

  // The first line that holds more than blanks.
  size_t lineStart = NO_OFFSET;
  for (size_t i = 0; i < length; i++) {
    const char c = text[i];
    if (c == '\n') {
      lineStart = i + 1;
    } else if (lineStart != NO_OFFSET && c != '\r' && c != ' ' && c != '\t') {
      return from + lineStart;
    }
  }

  // No line starts nearby: the code point at (or, at the very end, holding) `offset`.
  const size_t at = offset - from;
  size_t i = at;
  while (i < length && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80) i++;
  if (i == length) {
    i = at;
    while (i > 0 && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80) i--;
  }
  return from + i;
}

void TxtReaderActivity::jumpToPercent(const int percent) {
  const size_t fileSize = txt->getFileSize();
  if (fileSize == 0) return;
  size_t target = static_cast<size_t>(static_cast<uint64_t>(fileSize) * std::clamp(percent, 0, 100) / 100);
  if (target >= fileSize) target = fileSize - 1;
  if (pageIndex.covers(target)) {
    showOffset(target);
    return;
  }
  // Ahead of the index: open at a paragraph, never mid-line or mid-character.
  const size_t start = paragraphStartFrom(target);
  if (start != NO_OFFSET) {
    showOffset(start);
  }
}

bool TxtReaderActivity::loadPageAtOffset(const GfxRenderer& renderer, size_t offset,
                                         std::vector<DisplayLine>& outLines, size_t& nextOffset) {
  outLines.clear();

  PageMeasure measure{&renderer, cachedFontId, this};
  const auto layout = makeLayout(measure, viewportWidth, linesPerPage, cjkParagraphs, firstLineIndent);
  const TxtPagination::LineSink sink{&outLines, appendLine};

  const auto page = TxtPagination::loadPage(sourceWindow, offset, txt->getFileSize(), layout, &sink);
  if (page.status != TxtPagination::ReadStatus::Ok) {
    LOG_ERR("TRS", "Failed to read page at offset %zu (status %d)", offset, static_cast<int>(page.status));
    return false;
  }

  nextOffset = page.nextOffset;
  return true;
}

void TxtReaderActivity::renderBook() {
  if (!txt) {
    return;
  }

  if (!initialized) {
    initializeReader(renderer);
  }

  if (txt->getFileSize() == 0) {
    markBenchmarkRenderFailed();
    renderer.clearScreen();
    ReaderUtils::drawState(renderer, mappedInput, getBookTitle().c_str(), tr(STR_EMPTY_FILE));
    renderer.displayBuffer();
    return;
  }

  size_t nextOffset = 0;
  if (!loadPageAtOffset(renderer, currentOffset, currentPageLines, nextOffset)) {
    LOG_ERR("TRS", "Failed to load page at offset %zu", currentOffset);
    displayedOffset = NO_OFFSET;
    markBenchmarkRenderFailed();
    renderer.clearScreen();
    ReaderUtils::drawState(renderer, mappedInput, getBookTitle().c_str(), tr(STR_PAGE_LOAD_ERROR));
    renderer.displayBuffer();
    return;
  }
  displayedOffset = currentOffset;
  displayedNextOffset = nextOffset;

  updatePacingHint();

  renderer.clearScreen();
  renderPage(renderer);
  markPageRendered(0, static_cast<uint32_t>(displayedPageNumber() - 1),
                   static_cast<uint32_t>(bookProgressPercent() * 100.0f));

  // Save progress
  saveProgress();
}

int TxtReaderActivity::lineX(GfxRenderer& renderer, const DisplayLine& line) const {
  const char* text = line.text.c_str();
  int x = cachedOrientedMarginLeft;
  uint8_t effectiveAlignment = cachedParagraphAlignment;
  // A leading RTL run turns left/justified lines into right-aligned ones.
  if ((effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
       effectiveAlignment == CrossPointSettings::JUSTIFIED) &&
      BidiUtils::startsWithRtl(text, BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH)) {
    effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
  }

  // Apply text alignment; a paragraph's first line is laid out `indent` narrower.
  switch (effectiveAlignment) {
    case CrossPointSettings::LEFT_ALIGN:
    default:
      x += line.indent;
      break;
    case CrossPointSettings::CENTER_ALIGN: {
      const int textWidth = renderer.getTextAdvanceX(cachedFontId, text, EpdFontFamily::REGULAR);
      x = cachedOrientedMarginLeft + line.indent + (viewportWidth - line.indent - textWidth) / 2;
      break;
    }
    case CrossPointSettings::RIGHT_ALIGN: {
      const int textWidth = renderer.getTextAdvanceX(cachedFontId, text, EpdFontFamily::REGULAR);
      x = cachedOrientedMarginLeft + viewportWidth - textWidth;
      break;
    }
    case CrossPointSettings::JUSTIFIED:
      x += line.indent;
      break;
  }
  return x;
}

std::unique_ptr<ReaderWordPage> TxtReaderActivity::makeWordPage(GfxRenderer& renderer) {
  if (!initialized || currentPageLines.empty()) return nullptr;
  std::vector<TxtReaderWordPage::Line> lines;
  lines.reserve(currentPageLines.size());
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  int y = cachedOrientedMarginTop;
  for (const auto& line : currentPageLines) {
    if (!line.text.empty()) lines.push_back({line.text, lineX(renderer, line), y});
    y += lineHeight;
  }
  if (lines.empty()) return nullptr;
  return makeUniqueNoThrow<TxtReaderWordPage>(std::move(lines));
}

bool TxtReaderActivity::openDictionaryWordSelect() {
  if (!DictionaryRegistry::ensureSelected(SETTINGS.dictionaryName, sizeof(SETTINGS.dictionaryName))) {
    showDictionaryMessage = true;
    dictionaryMessageTime = millis();
    requestUpdate();
    return false;
  }
  std::unique_ptr<ReaderWordPage> page;
  {
    RenderLock lock;
    page = makeWordPage(renderer);
  }
  if (!page) return false;
  auto lookup = std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page),
                                                               cachedOrientedMarginLeft, cachedOrientedMarginTop);
  lookup->setReadingPosition(static_cast<uint32_t>(std::clamp(displayedPageNumber() - 1, 0, 0xFFFF)),
                             ConceptEngine::txtStoryCutoff(currentOffset));
  startActivityForResult(std::move(lookup), [this](const ActivityResult&) { requestUpdate(); });
  return true;
}

bool TxtReaderActivity::handleWordHold() {
  int x = 0;
  int y = 0;
  if (!initialized || isAtEndOfBook() || !SETTINGS.touchReaderControls || !mappedInput.hasTouch() ||
      !mappedInput.peekScreenLongPress(x, y)) {
    return false;
  }
  const auto zone = ReaderUtils::touchZoneAt(renderer, x, y, /*rtlBook=*/false);
  const bool zoneHoldActions = ReaderUtils::touchZoneHoldActions();
  if (ReaderTouchZones::zoneOwnsHold(zone, zoneHoldActions)) return false;

  // The hit test runs on the reader's own page before anything is pushed, so
  // a hold on blank space changes nothing on screen.
  std::unique_ptr<ReaderWordPage> page;
  std::vector<DictionaryPageWords::WordBox> words;
  uint16_t rowCount = 0;
  int hit = -1;
  {
    RenderLock lock;
    page = makeWordPage(renderer);
    if (!page) return false;
    const int fontId = SETTINGS.getReaderFontId();
    page->extractWords(renderer, fontId, 0, 0, words, rowCount);
    hit = DictionaryPageWords::wordNear(words, x, y, renderer.getLineHeight(fontId));
  }
  switch (ReaderTouchZones::holdAction(
      zone, zoneHoldActions, hit >= 0,
      hit >= 0 && DictionaryRegistry::ensureSelected(SETTINGS.dictionaryName, sizeof(SETTINGS.dictionaryName)))) {
    case ReaderTouchZones::HoldAction::None:
      return false;
    case ReaderTouchZones::HoldAction::NoDictionary:
      mappedInput.suppressScreenContact();
      showDictionaryMessage = true;
      dictionaryMessageTime = millis();
      requestUpdate();
      return true;
    case ReaderTouchZones::HoldAction::Lookup:
      break;
  }
  mappedInput.suppressScreenContact();
  auto lookup = makeUniqueNoThrow<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page), 0, 0,
                                                                std::move(words), rowCount, hit);
  if (!lookup) {
    LOG_ERR("TRS", "OOM: DictionaryWordSelectActivity");
    return true;
  }
  lookup->setReadingPosition(static_cast<uint32_t>(std::clamp(displayedPageNumber() - 1, 0, 0xFFFF)),
                             ConceptEngine::txtStoryCutoff(currentOffset));
  startActivityForResult(std::move(lookup), [this](const ActivityResult&) { requestUpdate(); });
  return true;
}

void TxtReaderActivity::renderPage(GfxRenderer& renderer) {
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.text.empty()) {
        renderer.drawText(cachedFontId, lineX(renderer, line), y, line.text.c_str());
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();      // scan pass
  renderStatusBar();  // scan: a CJK title joins the batch prewarm
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();

  const uint32_t frameDwellMs = takeFrameDwellMs();
  if (ReaderUtils::textAntiAliasingVisible(renderer)) {
    ReaderUtils::displayAdaptiveGrayscaleBase(renderer, refreshPolicy, frameDwellMs);
    if (!ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); })) {
      markBenchmarkRenderFailed();
    }
  } else {
    ReaderUtils::displayAdaptive(renderer, refreshPolicy, /*hasImage=*/false, /*async=*/false, frameDwellMs);
  }
  noteFrameDwell(frameDwellMs);

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  }
}

void TxtReaderActivity::noteFrameDwell(const uint32_t dwellMs) {
  if (isBenchmarkActivity()) return;
  const int page = std::clamp(displayedPageNumber() - 1, 0, 0xFFFF);
  auto& shown = dwellQueue.frameShown(dwellMs, static_cast<uint32_t>(page), millis());
  for (const auto& line : currentPageLines) {
    if (line.text.empty()) continue;
    if (!shown.append(line.text) || !shown.append(" ")) break;
  }
}

void TxtReaderActivity::recordDwellContext() {
  if (isBenchmarkActivity() || shouldYieldBackgroundIndexing()) return;
  ConceptEngine::PageDwellQueue::Page page;
  {
    // Never wait on a render; a turn still on its way (displayedOffset lags)
    // goes first.
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock() || displayedOffset != currentOffset ||
        !dwellQueue.take(page, millis(), ConceptEngine::DWELL_SETTLE_MS)) {
      return;
    }
  }
  ConceptEngine::recordPageDwell(bookPath, page.index, page.slice(), page.dwellMs);
}

size_t TxtReaderActivity::estimatedBytesPerPage() const {
  const auto& offsets = pageIndex.offsets();
  if (offsets.size() > 1 && pageIndex.frontier() > 0) {
    return std::max<size_t>(1, pageIndex.frontier() / (offsets.size() - 1));
  }
  if (displayedOffset != NO_OFFSET && displayedNextOffset != NO_OFFSET && displayedNextOffset > displayedOffset) {
    return displayedNextOffset - displayedOffset;
  }
  return TxtPagination::PAGE_INPUT_BYTES / 4;
}

int TxtReaderActivity::displayedPageNumber() const {
  if (pageIndex.covers(currentOffset)) {
    return static_cast<int>(pageIndex.pageContaining(currentOffset)) + 1;
  }
  // Ahead of the index: pages estimated from the bytes per page indexed so far.
  return static_cast<int>(pageIndex.offsets().size() +
                          (currentOffset - pageIndex.frontier()) / estimatedBytesPerPage());
}

int TxtReaderActivity::displayedPageCount() const {
  const size_t known = pageIndex.offsets().size();
  if (pageIndex.complete()) {
    return static_cast<int>(known);
  }
  const size_t fileSize = txt ? txt->getFileSize() : 0;
  const size_t bytesPerPage = estimatedBytesPerPage();
  const size_t remaining = fileSize > pageIndex.frontier() ? fileSize - pageIndex.frontier() : 0;
  const size_t estimate = known - 1 + std::max<size_t>(1, (remaining + bytesPerPage - 1) / bytesPerPage);
  return std::max(static_cast<int>(estimate), displayedPageNumber());
}

float TxtReaderActivity::bookProgressPercent() const {
  const size_t fileSize = txt ? txt->getFileSize() : 0;
  if (fileSize == 0) return 0;
  if (atEndOfBook) return 100;
  // Progress is the share of the text up to the end of the displayed page, as bytes.
  const size_t end = displayedOffset == currentOffset && displayedNextOffset != NO_OFFSET
                         ? std::min(displayedNextOffset, fileSize)
                         : currentOffset;
  return static_cast<float>(end) * 100.0f / static_cast<float>(fileSize);
}

int TxtReaderActivity::indexPercent() const {
  const size_t fileSize = txt ? txt->getFileSize() : 0;
  if (pageIndex.complete() || fileSize == 0) return 100;
  return static_cast<int>(static_cast<uint64_t>(pageIndex.frontier()) * 100 / fileSize);
}

void TxtReaderActivity::updatePacingHint() {
  pacingHint.clear();
  if (!txt || !initialized || atEndOfBook) {
    return;
  }
  // Nothing to draw on: skip the heading walk entirely.
  if (!SETTINGS.statusBarSpec().textLaneVisible(true)) {
    return;
  }
  // While the sweep is still running the page count is an estimate that keeps
  // growing, so a countdown now would jump; the status bar already marks that
  // state with "~".
  if (!pageIndex.complete()) {
    return;
  }
  const int pageCount = displayedPageCount();
  const int currentPage = displayedPageNumber() - 1;

  // Chapter span from the heading index: the last heading at or before this page
  // and the heading after it. No headings means the whole book is one chapter.
  int startPage = 0;
  int endPage = pageCount - 1;
  for (const uint32_t offset : pageIndex.chapters()) {
    const int page = static_cast<int>(pageIndex.pageContaining(offset));
    if (page <= currentPage) {
      startPage = std::max(startPage, page);
    } else {
      endPage = page - 1;
      break;
    }
  }
  if (!reading_pacing::isLongChapter(startPage, endPage)) {
    return;
  }

  const auto checkpoint = reading_pacing::nextCheckpoint(currentPage, startPage, endPage, nullptr, 0);
  char buffer[24];
  if (reading_pacing::formatCountdown(checkpoint, buffer, sizeof(buffer)) > 0) {
    pacingHint = buffer;
  }
}

void TxtReaderActivity::renderStatusBar() const {
  std::string title;
  if (SETTINGS.statusBarSpec().showsTitle()) {
    title = txt->getTitle();
  }
  // The micro-checkpoint countdown rides in the title lane, ahead of the book title
  // so it survives the lane's truncation.
  if (!pacingHint.empty() && SETTINGS.statusBarSpec().textLaneVisible(true)) {
    title = title.empty() ? pacingHint : pacingHint + "  " + title;
  }
  // While indexing, the page count is an estimate and carries the "~" marker.
  GUI.drawStatusBar(renderer, bookProgressPercent(), displayedPageNumber(), displayedPageCount(), title,
                    /*paddingBottom=*/0, /*textYOffset=*/0, /*fillMargin=*/true, /*isPageBookmarked=*/false,
                    /*pageCountEstimated=*/!pageIndex.complete());
}

bool TxtReaderActivity::handleFormatInput() {
  if (!txt || !initialized) {
    return false;
  }
  // The menu opens like the EPUB reader's: Confirm release or the touch menu gesture.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    openMenu();
    return true;
  }
  return false;
}

void TxtReaderActivity::openMenu() {
  TxtReaderMenuActivity::Status status;
  {
    RenderLock lock;
    status.page = displayedPageNumber();
    status.pageCount = displayedPageCount();
    status.bookPercent = static_cast<int>(bookProgressPercent() + 0.5f);
    status.indexing = !pageIndex.complete();
    status.indexPercent = indexPercent();
  }
  startActivityForResult(
      std::make_unique<TxtReaderMenuActivity>(renderer, mappedInput, getBookTitle(), status, boomerangRow()),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          return;
        }
        switch (static_cast<TxtReaderMenuActivity::MenuAction>(std::get<MenuResult>(result.data).action)) {
          case TxtReaderMenuActivity::MenuAction::QUICK_BREAK: {
            // While a break is open this row reads "Back to <book>"; otherwise it
            // opens one unread Inbox item. Progress for this page is saved by
            // renderBook(), so neither direction needs an extra write.
            if (returnFromBoomerang() || startBoomerangBreak()) {
              return;
            }
            break;
          }
          case TxtReaderMenuActivity::MenuAction::CONTENTS:
            openContents();
            break;
          case TxtReaderMenuActivity::MenuAction::GO_TO_PAGE:
            openPageSelection();
            break;
          case TxtReaderMenuActivity::MenuAction::GO_TO_PERCENT:
            openPercentSelection();
            break;
          case TxtReaderMenuActivity::MenuAction::DICTIONARY:
            openDictionaryWordSelect();
            break;
          case TxtReaderMenuActivity::MenuAction::REFRESH_SCREEN: {
            RenderLock lock;
            refreshPolicy.requestClear(AdaptiveRefreshPolicy::ClearSource::Manual);
            break;
          }
          case TxtReaderMenuActivity::MenuAction::FRONTLIGHT:
          case TxtReaderMenuActivity::MenuAction::READING_STATS:
            break;
        }
      });
}

void TxtReaderActivity::openContents() {
  // The index is not extended while the list is up (only the top activity loops), so the list may read it directly.
  startActivityForResult(std::make_unique<TxtReaderChapterSelectionActivity>(renderer, mappedInput, txt->getPath(),
                                                                             pageIndex, currentOffset, indexPercent()),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             openMenu();
                             return;
                           }
                           RenderLock lock;
                           showPage(static_cast<int>(std::get<PageResult>(result.data).page));
                         });
}

void TxtReaderActivity::openPageSelection() {
  int page;
  int pageCount;
  bool indexing;
  {
    RenderLock lock;
    page = displayedPageNumber();
    pageCount = displayedPageCount();
    indexing = !pageIndex.complete();
  }
  // The estimated range remains selectable while indexing; confirmation indexes through the requested page.
  startActivityForResult(std::make_unique<TxtReaderPageSelectionActivity>(renderer, mappedInput, page, pageCount,
                                                                          indexing),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             openMenu();
                             return;
                           }
                           RenderLock lock;
                           const int targetPage = static_cast<int>(std::get<PageResult>(result.data).page) - 1;
                           if (indexThrough(NO_OFFSET, targetPage)) {
                             showPage(targetPage);
                           }
                         });
}

void TxtReaderActivity::openPercentSelection() {
  int percent;
  {
    RenderLock lock;
    percent = static_cast<int>(bookProgressPercent() + 0.5f);
  }
  startActivityForResult(std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, percent),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             openMenu();
                             return;
                           }
                           RenderLock lock;
                           jumpToPercent(std::get<PercentResult>(result.data).percent);
                         });
}

bool TxtReaderActivity::pageTurn(bool isForward) {
  RenderLock lock;
  // Ignore paging until initializeReader has established the reading position
  if (!initialized) {
    return false;
  }
  return isForward ? stepForward() : stepBackward();
}

bool TxtReaderActivity::skipPages(int amount) {
  RenderLock lock;
  if (!initialized) {
    return false;
  }
  bool moved = false;
  for (int i = 0; i < std::abs(amount); i++) {
    // A forward skip stops at the end-of-book sentinel, as a forward turn does.
    if (amount > 0 && atEndOfBook) break;
    if (!(amount > 0 ? stepForward() : stepBackward())) break;
    moved = true;
  }
  return moved;
}

bool TxtReaderActivity::isAtEndOfBook() const { return initialized && atEndOfBook; }

void TxtReaderActivity::onReturnFromEndOfBook() { atEndOfBook = false; }

void TxtReaderActivity::saveProgress() const {
  if (isBenchmarkActivity()) return;
  uint8_t data[TxtProgressFormat::BYTES];
  TxtProgressFormat::encode(data, static_cast<uint32_t>(displayedPageNumber() - 1),
                            static_cast<uint32_t>(currentOffset), TxtPageIndexFormat::keyFingerprint(pageIndexKey()));
  if (!ProgressFile::writeAtomic(txt->getCachePath(), data, sizeof(data))) {
    LOG_ERR("TRS", "Failed to save progress: offset %zu", currentOffset);
  }
}

bool TxtReaderActivity::legacyPageOffset(const uint32_t page, uint32_t& offset) const {
  HalFile f;
  if (!Storage.openFileForRead("TRS", txt->getCachePath() + "/index.bin", f)) {
    return false;
  }
  const auto readAt = [&f](const size_t at, uint8_t* out, const size_t length) {
    return f.seek(at) && f.read(out, length) == static_cast<int>(length);
  };
  return TxtPageIndexFormat::legacyPageOffset(readAt, f.size(), static_cast<uint32_t>(txt->getFileSize()), page,
                                              offset);
}

void TxtReaderActivity::loadProgress() {
  showPage(0);
  if (isBenchmarkActivity()) return;

  HalFile f;
  if (!Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    return;
  }
  uint8_t data[TxtProgressFormat::BYTES];
  const int got = f.read(data, sizeof(data));
  f.close();
  const auto position = TxtProgressFormat::decode(data, got > 0 ? static_cast<size_t>(got) : 0);

  uint32_t offset = 0;
  switch (position.kind) {
    case TxtProgressFormat::Position::Kind::None:
      return;
    case TxtProgressFormat::Position::Kind::Offset:
      offset = position.offset;
      break;
    case TxtProgressFormat::Position::Kind::Page:
      // Saved by earlier firmware: its page index (still on disk until the
      // first save) turns the page into the text it showed.
      if (!legacyPageOffset(position.page, offset)) {
        // Only the page number survives: index up to it rather than reopen at the start.
        indexThrough(NO_OFFSET, static_cast<int>(position.page));
        showPage(static_cast<int>(position.page));
        LOG_DBG("TRS", "Loaded legacy progress: page %u", static_cast<unsigned>(position.page));
        return;
      }
      break;
  }

  if (offset >= txt->getFileSize()) {
    LOG_DBG("TRS", "Saved offset %u is past the end of the file", static_cast<unsigned>(offset));
    return;
  }
  showOffset(offset);
  LOG_DBG("TRS", "Loaded progress: offset %u (%s)", static_cast<unsigned>(offset),
          pageIndex.covers(offset) ? "indexed" : "ahead of the index");
}

TxtPageIndexFormat::Key TxtReaderActivity::pageIndexKey() const {
  TxtPageIndexFormat::Key key{};
  key.fileSize = static_cast<uint32_t>(txt->getFileSize());
  key.sourceSignature = txt->getSourceSignature();
  key.viewportWidth = viewportWidth;
  key.linesPerPage = linesPerPage;
  key.fontId = cachedFontId;
  key.screenMargin = cachedScreenMargin;
  key.paragraphAlignment = cachedParagraphAlignment;
  key.layoutFlags = cjkParagraphs ? TxtPageIndexFormat::LAYOUT_CJK_PARAGRAPHS : 0;
  key.firstLineIndent = static_cast<int16_t>(firstLineIndent);
  return key;
}

bool TxtReaderActivity::loadPageIndexCache() {
  using namespace TxtPageIndexFormat;

  HalFile f;
  if (!Storage.openFileForRead("TRS", txt->getCachePath() + "/index.bin", f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Header, exact length, checksum and every offset are checked before any of it is trusted.
  BufferedReader reader(f);
  Loaded loaded;
  const Check check = readIndex(reader, f.size(), pageIndexKey(), loaded);
  if (check != Check::Ok) {
    LOG_DBG("TRS", "Page index cache rejected (%s), rebuilding", describe(check));
    return false;
  }

  const bool complete = loaded.complete;
  pageIndex.adopt(txt->getFileSize(), std::move(loaded.offsets), std::move(loaded.chapters), complete);
  LOG_DBG("TRS", "Loaded %s page index cache: %zu pages", complete ? "complete" : "partial",
          pageIndex.offsets().size());
  return true;
}

void TxtReaderActivity::savePageIndexCache() {
  using namespace TxtPageIndexFormat;

  // Written beside the index and renamed into place, so an interrupted save leaves the old index (or none),
  // never a truncated one.
  const std::string finalPath = txt->getCachePath() + "/index.bin";
  const std::string tmpPath = finalPath + ".tmp";

  bool written = false;
  {
    HalFile f;
    if (!Storage.openFileForWrite("TRS", tmpPath, f)) {
      LOG_ERR("TRS", "Failed to save page index cache");
      return;
    }
    written = writeIndex([&f](const uint8_t* data, const size_t length) { return f.write(data, length) == length; },
                         pageIndexKey(), pageIndex.complete(), pageIndex.offsets(), pageIndex.chapters());
    written = written && f.sync();
    // f closes here, before the rename: SdFat must not rename a path that still has an open file.
  }

  if (!written) {
    LOG_ERR("TRS", "Failed to write page index cache");
    Storage.remove(tmpPath.c_str());
    return;
  }

  // SdFat's rename does not overwrite, so the old index goes first; a crash in between reads as "no cache".
  Storage.remove(finalPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("TRS", "Failed to move page index cache into place");
    Storage.remove(tmpPath.c_str());
    return;
  }

  pagesSinceSave = 0;
  LOG_DBG("TRS", "Saved %s page index cache: %zu pages", pageIndex.complete() ? "complete" : "partial",
          pageIndex.offsets().size());
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = displayedPageNumber();
  info.totalPages = displayedPageCount();
  info.progressPercent = std::min(100, static_cast<int>(bookProgressPercent() + 0.5f));
  return info;
}
