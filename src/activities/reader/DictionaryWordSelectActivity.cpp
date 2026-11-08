#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <string_view>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryDefinitionActivity.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "util/ConceptEngine.h"
#include "util/DictionaryRegistry.h"
#include "util/HtmlToPlainText.h"

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;
constexpr unsigned long WORD_REPEAT_START_MS = 500;
constexpr unsigned long WORD_REPEAT_INTERVAL_MS = 500;

void indexBuildYield(void*) { vTaskDelay(1); }

// Sense ranking and "Related" echoes read the words around the lookup; the
// episode recorded for later echoes is a wider window (about the encoder's
// 256-byte input).
constexpr int CONTEXT_WORDS = 8;
constexpr int EPISODE_WORDS = 24;

std::string escapeHtml(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (c == '&') {
      out += "&amp;";
    } else if (c == '<') {
      out += "&lt;";
    } else if (c == '>') {
      out += "&gt;";
    } else {
      out += c;
    }
  }
  return out;
}

// "★ " where the reader font draws U+2605; the built-in fonts stop at Latin
// and general punctuation, so they get "» " instead of a missing glyph.
const char* senseMarker(const GfxRenderer& renderer, const int fontId) {
  const auto& fonts = renderer.getFontMap();
  const auto family = fonts.find(fontId);
  return family != fonts.end() && family->second.hasCodepoint(0x2605) ? "\xE2\x98\x85 " : "\xC2\xBB ";
}

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  if (direct) {
    // The reader's frame stays up; loop() starts the lookup.
    directLookupPending = true;
    return;
  }
  // No null check: a failed allocation just disables the differential
  // fast path (drawHighlightWithSnapshot skips the read), keeping the
  // full-repaint path as the fallback.
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  if (page) page->extractWords(renderer, fontId, marginLeft, marginTop, words, rowCount);
  // Start on the middle row's word nearest mid-screen instead of top-left:
  // any word on the page is then at most half a page of moves away.
  if (!words.empty()) {
    const int initial = closestInRow(rowCount / 2, renderer.getScreenWidth() / 2);
    if (initial >= 0) selected = initial;
  }
  requestUpdate();
}

// Index of the word in `row` whose horizontal center is closest to centerX;
// -1 when the row has no words.
int DictionaryWordSelectActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

void DictionaryWordSelectActivity::moveVertical(const int direction) {
  const WordBox& current = words[selected];
  const int targetRow = static_cast<int>(current.row) + direction;
  if (targetRow < 0 || targetRow >= static_cast<int>(rowCount)) return;

  const int best = closestInRow(static_cast<uint16_t>(targetRow), current.x + current.width / 2);
  if (best >= 0 && best != selected) {
    selected = best;
    requestUpdate();
  }
}

void DictionaryWordSelectActivity::performLookup() {
  popup = Popup::Busy;
  popupMsg = StrId::STR_DICT_LOOKING_UP;
  requestUpdateAndWait();  // paint the busy popup before blocking on SD

  if (!installedScanned) {
    installedScanned = true;
    std::vector<DictionaryEntry> entries;
    DictionaryRegistry::discover(entries);
    installed.reserve(entries.size());
    for (auto& entry : entries) installed.push_back(std::move(entry.name));
  }

  // A held CJK character also offers the run one character earlier, so a
  // hold on the second character of 北京 still finds 北京.
  char run[CjkDictionary::RUN_BYTES];
  char previous[CjkDictionary::RUN_BYTES];
  DictionaryPageWords::heldCjkRuns(words, selected, run, previous);
  std::vector<std::string> names;
  DictionaryRouter::order(installed, SETTINGS.dictionaryName, run[0] != '\0', names);

  std::string definition;
  std::string headword;
  uint8_t matchMask = 0;
  bool usedPrevious = false;
  DictionaryRouter::Miss miss = DictionaryRouter::Miss::NoDictionary;
  const auto beforeIndex = [this] {
    popupMsg = StrId::STR_DICT_INDEXING;
    requestUpdateAndWait();
  };
  if (router.lookup(names, words[selected].text, run, previous, beforeIndex, &indexBuildYield, definition, headword,
                    matchMask, usedPrevious, miss)) {
    popup = Popup::None;
    Dictionary& dict = router.dictionary();
    const char* shown = usedPrevious ? previous : run;
    addConceptNotes(definition, dict.definitionsAreHtml());
    startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(
                               renderer, mappedInput, std::move(headword), std::move(definition),
                               dict.definitionsAreHtml(), run[0] ? &dict : nullptr, shown, matchMask),
                           [this](const ActivityResult&) {
                             if (direct) {
                               finish();
                             } else {
                               requestUpdate();
                             }
                           });
    return;
  }
  // No dictionary knows the word (or none is installed): the Engram table may
  // still hold a one-line gloss for it (slang, idioms, compounds). A CJK hold
  // tries the compounds starting at the held character, then those starting
  // one earlier that still cover it.
  if (miss == DictionaryRouter::Miss::NotFound || miss == DictionaryRouter::Miss::NoDictionary) {
    std::string word;
    std::string gloss;
    if (run[0]) {
      gloss = ConceptEngine::lookupEngramGloss(run, &word);
      if (gloss.empty() && previous[0]) gloss = ConceptEngine::lookupEngramGloss(previous, &word, 2);
    } else if (words[selected].text) {
      gloss = ConceptEngine::lookupEngramGloss(words[selected].text, &word);
    }
    if (!gloss.empty()) {
      popup = Popup::None;
      addConceptNotes(gloss, false);
      startActivityForResult(
          std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(word), std::move(gloss)),
          [this](const ActivityResult&) {
            if (direct) {
              finish();
            } else {
              requestUpdate();
            }
          });
      return;
    }
  }
  // Name the failure: a genuine miss is "Not found"; a word that WAS found but
  // couldn't be read is a real error — and we distinguish decompression from a
  // low-memory allocation from a generic read error.
  popup = Popup::Error;
  switch (miss) {
    case DictionaryRouter::Miss::NotFound:
      popup = Popup::NotFound;
      popupMsg = StrId::STR_DICT_NOT_FOUND;
      break;
    case DictionaryRouter::Miss::NoDictionary:
      popupMsg = StrId::STR_DICT_NO_DICT_SET;
      break;
    case DictionaryRouter::Miss::Decompress:
      popupMsg = StrId::STR_DICT_DECOMPRESS_ERROR;
      break;
    case DictionaryRouter::Miss::IndexLowMemory:
    case DictionaryRouter::Miss::LowMemory:
      popupMsg = StrId::STR_DICT_LOW_MEMORY;
      break;
    case DictionaryRouter::Miss::IndexReadError:
    case DictionaryRouter::Miss::ReadError:
      popupMsg = StrId::STR_DICT_READ_FAILED;
      break;
    case DictionaryRouter::Miss::OpenFailed:
    default:
      popupMsg = StrId::STR_DICT_ERROR;
      break;
  }
  popupTime = millis();
  requestUpdate();
}

// The words within `radius` of the selected one, in reading order. CJK
// characters join without spaces.
std::string DictionaryWordSelectActivity::contextAround(const int radius) const {
  std::string text;
  if (words.empty()) return text;
  const int first = std::max(0, selected - radius);
  const int last = std::min(static_cast<int>(words.size()) - 1, selected + radius);
  for (int i = first; i <= last; ++i) {
    if (!words[i].text) continue;
    const bool cjk = words[i].cjkBytes != 0;
    if (!text.empty() && !(cjk && i > first && words[i - 1].cjkBytes)) text += ' ';
    if (cjk) {
      text.append(words[i].text, words[i].cjkBytes);
    } else {
      text += words[i].text;
    }
  }
  return text;
}

// Concept-engine extras for a definition about to be shown: the sense that
// fits the passage marked at the top, a "Related:" echo of the closest page
// read before (or Inbox item) at cosine >= 0.65, and this passage recorded
// for later echoes.
void DictionaryWordSelectActivity::addConceptNotes(std::string& definition, const bool html) {
  const std::string context = contextAround(CONTEXT_WORDS);
  int8_t contextVec[ConceptEngine::DIM];
  ConceptEngine::encodeText(context, contextVec);
  const char* marker = senseMarker(renderer, fontId);
  if (html) {
    // Styled HTML keeps its markup: the best sense is repeated above it.
    const std::string best = ConceptEngine::bestDictionarySense(contextVec, htmlToPlainText(definition));
    if (!best.empty()) definition = "<p><b>" + escapeHtml(marker) + escapeHtml(best) + "</b></p>" + definition;
  } else {
    definition = ConceptEngine::rankDictionarySenses(contextVec, definition, marker);
  }

  const std::string& bookPath = APP_STATE.openEpubPath;
  const uint64_t book = ConceptEngine::fnv1a64(bookPath);
  for (const auto& match : ConceptEngine::queryEpisodicAndShelf(contextVec, 4)) {
    if (match.dot < ConceptEngine::RELATED_MIN_DOT) break;
    if (match.pathHash == book && (match.shelf || !hasReadingPosition || match.page == readingPosition)) continue;
    const std::string line = "Related: " + ConceptEngine::describeMatch(match);
    definition += html ? "<p>" + escapeHtml(line) + "</p>" : "\n\n" + line;
    break;
  }
  if (hasReadingPosition && !bookPath.empty()) {
    ConceptEngine::appendEpisodicPage(bookPath, readingPosition, contextAround(EPISODE_WORDS));
  }
}

void DictionaryWordSelectActivity::loop() {
  if (directLookupPending) {
    directLookupPending = false;
    performLookup();
    return;
  }
  if (popup == Popup::NotFound || popup == Popup::Error) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      if (direct) {
        finish();
        return;
      }
      popup = Popup::None;
      requestUpdate();
    }
    return;
  }
  if (direct) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !words.empty()) {
    performLookup();
    return;
  }

  if (words.empty()) return;

  // Touch: the highlight follows the finger from the first frame of contact
  // (and while dragging), so a press visibly selects; a tap on a word then
  // looks it up on release.
  int tx = 0;
  int ty = 0;
  if (mappedInput.isScreenTouchHeld(tx, ty)) {
    const int hit = DictionaryPageWords::wordAt(words, tx, ty, lineHeight);
    if (hit >= 0 && hit != selected) {
      selected = hit;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const int hit = DictionaryPageWords::wordAt(words, tx, ty, lineHeight);
    if (hit >= 0) {
      selected = hit;
      performLookup();
    }
    return;
  }

  const bool hasNextWord = selected + 1 < static_cast<int>(words.size());
  const unsigned long now = millis();
  const bool repeat =
      mappedInput.getHeldTime() >= WORD_REPEAT_START_MS && now - lastHorizontalMoveTime >= WORD_REPEAT_INTERVAL_MS;
  const bool moveLeft = mappedInput.wasPressed(MappedInputManager::Button::ScreenLeft) ||
                        (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenLeft));
  const bool moveRight = mappedInput.wasPressed(MappedInputManager::Button::ScreenRight) ||
                         (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenRight));
  if (moveLeft && selected > 0) {
    selected--;
    lastHorizontalMoveTime = now;
    requestUpdate();
  } else if (moveRight && hasNextWord) {
    selected++;
    lastHorizontalMoveTime = now;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown)) {
    moveVertical(1);
  }
}

// Saves the pixels under words[selected]'s highlight box, then draws the
// highlight over them. Returns false when the pixels could not be saved
// (no buffer / oversize box) — the highlight is drawn regardless, but the
// next cursor move must do a full repaint.
bool DictionaryWordSelectActivity::drawHighlightWithSnapshot() {
  const WordBox& word = words[selected];
  int hx = word.x - 2;
  int hy = word.y - 2;
  int hw = word.width + 4;
  int hh = lineHeight + 4;
  // Clamp to the panel so save, draw and restore all use the same box.
  if (hx < 0) {
    hw += hx;
    hx = 0;
  }
  if (hy < 0) {
    hh += hy;
    hy = 0;
  }

  bool saved = false;
  if (snapshot && hw > 0 && hh > 0) {
    saved = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  }
  snapshotX = static_cast<int16_t>(hx);
  snapshotY = static_cast<int16_t>(hy);
  snapshotW = static_cast<int16_t>(hw);
  snapshotH = static_cast<int16_t>(hh);
  snapshotIdx = saved ? selected : -1;

  renderer.fillRect(hx, hy, hw, hh, true);
  renderer.drawText(fontId, word.x, word.y, word.text, false, word.style);
  return saved;
}

// Front-button bar (Back/Confirm/Left/Right). Drawn last on every repaint
// path, including the differential highlight-only path, so it always ends
// up as the top layer even when a highlighted word's box falls under a
// hint's screen area. No side-button hints: the full-bleed reader page has no
// spare gutter for them, so a hint box there would hide text.
void DictionaryWordSelectActivity::drawHints() const {
  // No selectable word on this page: Confirm and navigation are all no-ops
  // (guarded by words.empty() in loop()/performLookup), so only Back does
  // anything and only Back is hinted.
  if (words.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_LOOKUP), tr(STR_DIR_LEFT),
                                                       tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// Same prewarm-scan-then-render pass the reader uses, so SD-card fonts hit
// the in-RAM glyph cache during the real draw.
void DictionaryWordSelectActivity::drawPage() {
  renderer.clearScreen();
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  if (page) page->render(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  if (page) page->render(renderer, fontId, marginLeft, marginTop);
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  // Direct mode draws no picker: the busy popup goes straight over the
  // reader's frame. A failure popup gets the page redrawn under it, with the
  // word highlighted, so it does not land on top of the busy popup.
  if (direct) {
    if (popup == Popup::None) return;
    if (popup != Popup::Busy) {
      drawPage();
      drawHighlightWithSnapshot();
    }
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }

  // Differential fast path: only the highlight moved and the framebuffer
  // still holds a clean page (no popup or sub-activity since the last full
  // repaint). Restore the pixels under the old highlight, draw the new one,
  // and push — skipping the two-pass page render entirely.
  if (popup == Popup::None && snapshotIdx >= 0 && !words.empty() && selected != snapshotIdx) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    // The full path's PrewarmScope cleared the glyph cache on exit; batch-load
    // just the highlighted word's glyphs before drawing them white-on-black.
    renderer.getFontCacheManager()->prewarmCache(
        fontId, words[selected].text, static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[selected].style) & 0x03)));
    if (drawHighlightWithSnapshot()) {
      drawHints();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    // Snapshot failed (oversize box) — fall through to a full repaint.
  }

  if (words.empty()) {
    renderer.clearScreen();
    ReaderUtils::drawState(renderer, mappedInput, tr(STR_DICTIONARY), tr(STR_DICT_NO_WORDS));
    renderer.displayBuffer();
    return;
  }

  drawPage();

  if (!words.empty()) {
    drawHighlightWithSnapshot();
  }

  drawHints();

  if (popup != Popup::None) {
    // The popup overdraws the page, so the snapshot no longer matches the
    // framebuffer — force the next render onto the full-repaint path.
    snapshotIdx = -1;
    // drawPopup overlays the framebuffer and refreshes the display itself.
    // I18N.get directly: tr() only accepts literal key names.
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
