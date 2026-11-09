#include "DictionaryDefinitionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictHtmlPages.h"
#include "util/HtmlToPlainText.h"

namespace {

// Longest measurable/drawable span. Wrapped lines stay under the screen width
// (far below this); only pathological unbreakable tokens are split at this cap.
constexpr size_t MAX_LINE_BYTES = 191;

// Styled-path ceiling: the laid-out Pages keep the whole definition resident
// (TextBlock arenas ≈ text + ~7 bytes/word plus per-line objects), roughly
// doubling the string's footprint while this activity is stacked over the
// reader and word-select. Bigger definitions take the span-based plain-text
// path, which holds no per-page copies.
constexpr size_t MAX_STYLED_HTML_BYTES = 16 * 1024;

// Rows above the body: the CJK match bar, then (sectioned view) the tabs and
// the length toggle.
constexpr int MATCH_BAR_HEIGHT = 44;
constexpr int TAB_ROW_HEIGHT = 40;
constexpr int TOGGLE_ROW_HEIGHT = 36;
constexpr int ROW_GAP = 6;
// Holding Confirm this long toggles Compact / Longer; a short press is the
// next tab.
constexpr unsigned long LENGTH_TOGGLE_MS = 700;
// Section header rules: a short lead-in, then the rest of the line.
constexpr int HEADER_LEAD_RULE = 16;
constexpr int HEADER_RULE_GAP = 8;

using DictionaryLookup::LineKind;
using DictionaryLookup::Section;

}  // namespace

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  layoutDefinition();
  requestUpdate();
}

void DictionaryDefinitionActivity::layoutDefinition() {
  pages.clear();
  lines.clear();
  currentPage = 0;
  totalPages = 1;
  if (dictionary) definition.insert(0, headword + "\n\n");
  // Normalize StarDict multi-type separators so the wrap loop and the
  // C-string font APIs below both see the whole definition.
  std::replace(definition.begin(), definition.end(), '\0', '\n');
  if (sectioned) {
    // The tabs show plain text: the definition becomes the Def section.
    sections.defLong = htmlToPlainText(definition);
    sections.defShort = DictionaryLookup::compactDefinition(sections.defLong);
    if (sections.defShort == sections.defLong) sections.defShort.clear();
    layoutSections();
    return;
  }
  if (!(htmlDefinition && definition.size() <= MAX_STYLED_HTML_BYTES && layoutHtmlPages())) {
    definition = htmlToPlainText(definition);
    wrapText();
  }
}

void DictionaryDefinitionActivity::layoutSections() {
  if (!sections.has(activeSection)) activeSection = Section::All;
  definition = DictionaryLookup::composeSections(sections, activeSection, longer, headerOffsets);
  wrapText();
  std::vector<LineKind> kinds;
  kinds.reserve(lines.size());
  for (size_t i = 0; i < lines.size(); ++i) {
    kinds.push_back(isHeaderLine(i) ? LineKind::Header : lines[i].len == 0 ? LineKind::Blank : LineKind::Text);
  }
  pageStarts = DictionaryLookup::paginateLines(kinds, linesPerPage);
  totalPages = static_cast<int>(pageStarts.size());
  currentPage = 0;
}

void DictionaryDefinitionActivity::selectSection(const Section section) {
  if (section == activeSection) return;
  {
    RenderLock lock;
    activeSection = section;
    layoutSections();
  }
  requestUpdate();
}

void DictionaryDefinitionActivity::setLonger(const bool value) {
  if (value == longer) return;
  {
    RenderLock lock;
    longer = value;
    layoutSections();
  }
  requestUpdate();
}

size_t DictionaryDefinitionActivity::availableSections(Section out[DictionaryLookup::SECTION_COUNT]) const {
  size_t n = 0;
  for (const Section section : {Section::All, Section::Def, Section::Story, Section::Related, Section::Riff}) {
    if (sections.has(section)) out[n++] = section;
  }
  return n;
}

bool DictionaryDefinitionActivity::isHeaderLine(const size_t line) const {
  return sectioned && line < lines.size() && lines[line].len > 0 &&
         std::binary_search(headerOffsets.begin(), headerOffsets.end(), lines[line].start);
}

int DictionaryDefinitionActivity::pageFirstLine(const int page) const {
  if (!sectioned) return page * linesPerPage;
  return page >= 0 && page < static_cast<int>(pageStarts.size()) ? static_cast<int>(pageStarts[page])
                                                                 : static_cast<int>(lines.size());
}

int DictionaryDefinitionActivity::pageEndLine(const int page) const {
  const int first = pageFirstLine(page);
  int end = std::min(first + linesPerPage, static_cast<int>(lines.size()));
  if (sectioned && page + 1 < static_cast<int>(pageStarts.size())) {
    end = std::min(end, static_cast<int>(pageStarts[page + 1]));
  }
  return end;
}

void DictionaryDefinitionActivity::changeMatch(const int direction) {
  RenderLock lock;
  uint8_t ends[CjkDictionary::MAX_CHARACTERS];
  const int count = CjkDictionary::prefixes(cjkRun, ends);
  int current = 0;
  while (current < count && ends[current] != headword.size()) ++current;
  for (int next = current + direction; next >= 0 && next < count; next += direction) {
    if (!(matchMask & (1u << next))) continue;
    char candidate[CjkDictionary::RUN_BYTES];
    memcpy(candidate, cjkRun, ends[next]);
    candidate[ends[next]] = '\0';
    std::string text;
    std::string matched;
    Dictionary::LookupResult result;
    if (dictionary->lookup(candidate, text, matched, &result)) {
      headword = std::move(matched);
      definition = std::move(text);
      lookupFailed = false;
      layoutDefinition();
    } else {
      lookupFailed = true;
      lookupError = result == Dictionary::LookupResult::LowMemory    ? StrId::STR_DICT_LOW_MEMORY
                    : result == Dictionary::LookupResult::Decompress ? StrId::STR_DICT_DECOMPRESS_ERROR
                                                                     : StrId::STR_DICT_READ_FAILED;
    }
    requestUpdate();
    return;
  }
}

void DictionaryDefinitionActivity::onExit() {
  Activity::onExit();
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }
}

int DictionaryDefinitionActivity::controlsHeight() const {
  return (dictionary ? MATCH_BAR_HEIGHT : 0) + (sectioned ? TAB_ROW_HEIGHT + ROW_GAP + TOGGLE_ROW_HEIGHT + ROW_GAP : 0);
}

DictionaryDefinitionActivity::BodyArea DictionaryDefinitionActivity::bodyArea() const {
  const Rect body = UITheme::getContentBounds(renderer);
  return {body.width, body.height - controlsHeight()};
}

// Styled path: lay the HTML definition out through the EPUB chapter parser
// into reader-identical Pages. Frees `definition` on success (the page arenas
// own the text); any failure leaves state untouched for the plain-text path.
bool DictionaryDefinitionActivity::layoutHtmlPages() {
  const BodyArea body = bodyArea();
  if (body.width <= 0 || body.height <= 0) return false;
  if (!buildDictionaryHtmlPages(renderer, definition, static_cast<uint16_t>(body.width),
                                static_cast<uint16_t>(body.height), pages)) {
    return false;
  }
  definition.clear();
  definition.shrink_to_fit();
  totalPages = static_cast<int>(pages.size());
  currentPage = 0;
  return true;
}

int DictionaryDefinitionActivity::measureSpan(const int fontId, const char* text, size_t len) const {
  char buf[MAX_LINE_BYTES + 1];
  len = std::min(len, MAX_LINE_BYTES);
  memcpy(buf, text, len);
  buf[len] = '\0';
  return renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR);
}

// Greedy word-wrap of `definition` into byte spans. '\n' breaks lines (blank
// lines survive as paragraph spacing; NULs from multi-type StarDict entries
// were normalized to newlines in onEnter); '\r' is dropped by treating it as
// a space at a token edge.
void DictionaryDefinitionActivity::wrapText() {
  lines.clear();
  lines.reserve(definition.size() / 32 + 8);

  const int fontId = SETTINGS.getReaderFontId();
  // SD-card fonts: merge every definition codepoint into the persistent
  // advance table up front. Otherwise each unseen codepoint measured below
  // falls back to an on-demand glyph load from SD (8-slot overflow ring).
  // Sectioned view: its headers are drawn bold.
  renderer.ensureSdCardFontReady(fontId, definition.c_str(), sectioned ? 0x03 /* REGULAR | BOLD */ : 0x01);

  const BodyArea body = bodyArea();
  const int maxWidth = body.width;
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  const int lineHeight = renderer.getLineHeight(fontId);
  linesPerPage = std::max(1, body.height / lineHeight);

  const char* text = definition.c_str();
  const uint32_t n = static_cast<uint32_t>(definition.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;

  const auto flushLine = [&](uint32_t nextStart) {
    lines.push_back({lineStart, static_cast<uint16_t>(lineEnd - lineStart)});
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
  };

  uint32_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (c == '\n' || c == '\0') {
      flushLine(i + 1);
      i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }

    // Token: run of non-whitespace bytes, capped at the measure buffer.
    const uint32_t tokenStart = i;
    while (i < n && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' && text[i] != '\0' &&
           i - tokenStart < MAX_LINE_BYTES) {
      i++;
    }
    // If the byte cap cut the token mid-UTF-8-sequence, back off to the last
    // complete codepoint so measure/draw never see a partial sequence. A
    // natural stop lands on whitespace or the terminating NUL, never on a
    // continuation byte, so this is a no-op there.
    while (i - tokenStart > 1 && (text[i] & 0xC0) == 0x80) i--;
    const uint32_t tokenLen = i - tokenStart;
    const int tokenWidth = measureSpan(fontId, text + tokenStart, tokenLen);

    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    } else if (lineWidth + spaceWidth + tokenWidth <= maxWidth &&
               tokenStart + tokenLen - lineStart <= UINT16_MAX) {  // span len must fit Line::len
      lineEnd = tokenStart + tokenLen;
      lineWidth += spaceWidth + tokenWidth;
    } else {
      flushLine(tokenStart);
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    }

    // An unbreakable token wider than the screen is now alone on the line
    // (any previous content was flushed above): split it at the widest
    // fitting UTF-8 boundary and carry the remainder forward.
    while (lineWidth > maxWidth && lineEnd - lineStart > 1) {
      const uint32_t len = lineEnd - lineStart;
      uint32_t lastFit = 0;
      for (uint32_t f = 1; f <= len; f++) {
        if (f == len || (text[lineStart + f] & 0xC0) != 0x80) {  // codepoint boundary
          if (measureSpan(fontId, text + lineStart, f) > maxWidth) break;
          lastFit = f;
        }
      }
      if (lastFit == 0) {
        // Even a single over-wide glyph must make progress; consume its whole
        // UTF-8 sequence rather than splitting it into invalid fragments.
        lastFit = 1;
        while (lastFit < len && (text[lineStart + lastFit] & 0xC0) == 0x80) lastFit++;
      }
      const uint32_t rest = lineStart + lastFit;
      lineEnd = rest;
      flushLine(rest);
      lineEnd = rest + (len - lastFit);
      lineWidth = measureSpan(fontId, text + lineStart, lineEnd - lineStart);
    }
  }
  if (lineEnd > lineStart) flushLine(n);

  // Trim trailing blank lines so the last page is not empty padding.
  while (!lines.empty() && lines.back().len == 0) lines.pop_back();

  totalPages = std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
  currentPage = 0;
}

void DictionaryDefinitionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (sectioned) {
    // Hold Confirm: Compact <-> Longer; press: the next tab with text.
    if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LENGTH_TOGGLE_MS)) {
      setLonger(!longer);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      selectSection(DictionaryLookup::nextSection(sections, activeSection, 1));
      return;
    }
    if (!dictionary && mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      setLonger(false);
      return;
    }
    if (!dictionary && mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      setLonger(true);
      return;
    }
  }
  if (dictionary && mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    changeMatch(-1);
    return;
  }
  if (dictionary && mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    changeMatch(1);
    return;
  }

  // Same tap zones as the reader page turns: left third = previous page,
  // the rest = next. Back is the usual left-edge swipe.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const Rect content = UITheme::getContentBounds(renderer);
    int rowY = content.y;
    if (dictionary) {
      if (ty < rowY + MATCH_BAR_HEIGHT) {
        changeMatch(tx < renderer.getScreenWidth() / 2 ? -1 : 1);
        return;
      }
      rowY += MATCH_BAR_HEIGHT;
    }
    if (sectioned && ty >= rowY && ty < rowY + TAB_ROW_HEIGHT + ROW_GAP / 2) {
      Section available[DictionaryLookup::SECTION_COUNT];
      const size_t n = availableSections(available);
      if (n > 0 && content.width > 0) {
        const int cell = std::clamp((tx - content.x) * static_cast<int>(n) / content.width, 0, static_cast<int>(n) - 1);
        selectSection(available[cell]);
      }
      return;
    }
    rowY += TAB_ROW_HEIGHT + ROW_GAP;
    if (sectioned && ty >= rowY - ROW_GAP / 2 && ty < rowY + TOGGLE_ROW_HEIGHT + ROW_GAP / 2) {
      setLonger(tx >= content.x + content.width / 2);
      return;
    }
    if (tx < renderer.getScreenWidth() / 3) {
      if (currentPage > 0) {
        currentPage--;
        requestUpdate();
      }
    } else if (currentPage + 1 < totalPages) {
      currentPage++;
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onNext([this] {
    if (currentPage + 1 < totalPages) {
      currentPage++;
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (currentPage > 0) {
      currentPage--;
      requestUpdate();
    }
  });
}

// Draws the current page: a styled Page when the HTML layout succeeded,
// otherwise the wrapped line spans (copied into a stack buffer for NUL
// termination). Called twice per render: once in font-cache scan mode, once
// for the real paint.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY, const int width) const {
  if (!pages.empty()) {
    pages[currentPage]->render(renderer, fontId, x, startY);
    return;
  }
  const int lineHeight = renderer.getLineHeight(fontId);
  char buf[MAX_LINE_BYTES + 1];
  const int firstLine = pageFirstLine(currentPage);
  const int lastLine = pageEndLine(currentPage);
  for (int i = firstLine; i < lastLine; i++) {
    if (lines[i].len == 0) continue;
    const size_t len = std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES);
    memcpy(buf, definition.c_str() + lines[i].start, len);
    buf[len] = '\0';
    const int y = startY + (i - firstLine) * lineHeight;
    if (!isHeaderLine(static_cast<size_t>(i))) {
      renderer.drawText(fontId, x, y, buf);
      continue;
    }
    // "── HEADER ─────": bold, ruled out to the right edge.
    const int ruleY = y + lineHeight / 2;
    const int textX = x + HEADER_LEAD_RULE + HEADER_RULE_GAP;
    renderer.drawLine(x, ruleY, x + HEADER_LEAD_RULE, ruleY, 2, true);
    renderer.drawText(fontId, textX, y, buf, true, EpdFontFamily::BOLD);
    const int ruleX = textX + renderer.getTextWidth(fontId, buf, EpdFontFamily::BOLD) + HEADER_RULE_GAP;
    if (ruleX < x + width) renderer.drawLine(ruleX, ruleY, x + width - 1, ruleY, 2, true);
  }
}

// The tab row (active tab inverted, only tabs with text) and the
// Compact | Longer toggle (active half inverted).
void DictionaryDefinitionActivity::drawSectionControls(const int x, const int y, const int width) const {
  const int smallHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const auto drawRow = [&](const int rowY, const int rowHeight, const char* const* labels, const size_t count,
                           const size_t active) {
    renderer.drawRect(x, rowY, width, rowHeight);
    const int textY = rowY + (rowHeight - smallHeight) / 2;
    for (size_t k = 0; k < count; ++k) {
      const int cellX = x + static_cast<int>(k) * width / static_cast<int>(count);
      const int cellW = x + static_cast<int>(k + 1) * width / static_cast<int>(count) - cellX;
      const bool on = k == active;
      if (on) {
        renderer.fillRect(cellX, rowY, cellW, rowHeight, true);
      } else if (k > 0 && k != active + 1) {
        renderer.drawLine(cellX, rowY, cellX, rowY + rowHeight - 1, true);
      }
      UITheme::drawCenteredText(renderer, Rect{cellX, rowY, cellW, rowHeight}, SMALL_FONT_ID, textY, labels[k], !on,
                                on ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    }
  };
  Section available[DictionaryLookup::SECTION_COUNT];
  const char* tabLabels[DictionaryLookup::SECTION_COUNT];
  const size_t count = availableSections(available);
  size_t active = 0;
  for (size_t k = 0; k < count; ++k) {
    tabLabels[k] = DictionaryLookup::sectionLabel(available[k]);
    if (available[k] == activeSection) active = k;
  }
  drawRow(y, TAB_ROW_HEIGHT, tabLabels, count, active);
  static const char* const LENGTH_LABELS[] = {"Compact", "Longer"};
  drawRow(y + TAB_ROW_HEIGHT + ROW_GAP, TOGGLE_ROW_HEIGHT, LENGTH_LABELS, 2, longer ? 1 : 0);
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  Rect body = UITheme::getContentBounds(renderer);
  char counter[16] = {};
  if (totalPages > 1) snprintf(counter, sizeof(counter), "%d/%d", currentPage + 1, totalPages);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 lookupFailed ? I18N.get(lookupError) : (dictionary ? tr(STR_LOOKUP) : headword.c_str()),
                 counter[0] ? counter : nullptr);
  if (dictionary) {
    const int half = body.width / 2;
    UITheme::drawCenteredText(renderer, Rect{body.x, body.y, half, MATCH_BAR_HEIGHT}, SMALL_FONT_ID, body.y,
                              tr(STR_DICT_SHORTER));
    UITheme::drawCenteredText(renderer, Rect{body.x + half, body.y, body.width - half, MATCH_BAR_HEIGHT}, SMALL_FONT_ID,
                              body.y, tr(STR_DICT_LONGER));
    body.y += MATCH_BAR_HEIGHT;
  }
  if (sectioned) {
    drawSectionControls(body.x, body.y, body.width);
    body.y += TAB_ROW_HEIGHT + ROW_GAP + TOGGLE_ROW_HEIGHT + ROW_GAP;
  }

  // Body: two-pass draw inside a prewarm scope (same pattern as the reader's
  // renderContents) so SD-card font glyphs load from SD in one batch instead
  // of one on-demand overflow read per character on every page turn.
  const int fontId = SETTINGS.getReaderFontId();
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawBody(fontId, body.x, body.y, body.width);  // scan pass: records codepoints only
  scope.endScanAndPrewarm();
  drawBody(fontId, body.x, body.y, body.width);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), sectioned ? "Tab" : "", (currentPage > 0 ? "<" : ""),
                                            (currentPage + 1 < totalPages ? ">" : ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
