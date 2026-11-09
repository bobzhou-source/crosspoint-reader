#pragma once

#include <I18n.h>

#include <memory>
#include <string>
#include <vector>

#include "DictionaryLookupSections.h"
#include "DictionaryPageWords.h"
#include "ReaderWordPage.h"
#include "activities/Activity.h"
#include "util/DictionaryRouter.h"

// Word selection over the current reader page: Left/Right step through words
// in reading order, Up/Down jump rows, Confirm looks the word up and opens
// DictionaryDefinitionActivity, Back returns to the reader. On touch devices a
// touch-down moves the highlight and a tap on a word looks it up directly.
//
// Direct mode (press and hold in the reader) skips the picker: it looks up
// the word the reader hit, draws only its popups over the reader's frame, and
// finishes when the definition or a failure popup closes.
class DictionaryWordSelectActivity final : public Activity {
 public:
  using WordBox = DictionaryPageWords::WordBox;

  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<ReaderWordPage> page, int marginLeft, int marginTop)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop) {}

  // Direct mode: `words` were extracted from `page` and words[selected] is the
  // word to look up.
  DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::unique_ptr<ReaderWordPage> page, int marginLeft, int marginTop,
                               std::vector<WordBox>&& words, uint16_t rowCount, int selected)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        direct(true),
        words(std::move(words)),
        selected(selected),
        rowCount(rowCount) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Where the page sits in the open book (ConceptEngine::epubPosition for
  // EPUB, the page index for TXT) and its layout-independent story cutoff
  // (ConceptEngine::epubStoryCutoff / txtStoryCutoff), the spoiler gate for
  // story refreshers. Set before start; without it lookups are not recorded
  // in the episodic index and no story refresher is shown.
  void setReadingPosition(uint32_t position, uint32_t cutoff) {
    readingPosition = position;
    storyCutoff = cutoff;
    hasReadingPosition = true;
  }
  // The open book's title, used to label saved margin notes ("Title ch.3
  // p.12 — word: riff"); without it the file name is used.
  void setBookTitle(std::string title) { bookTitle = std::move(title); }

 private:
  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  int closestInRow(uint16_t row, int centerX) const;
  void moveVertical(int direction);
  void performLookup();
  void drawPage();
  bool drawHighlightWithSnapshot();
  void drawHints() const;
  std::string contextAround(int radius) const;
  std::string weaveStory(std::string& matched) const;
  // The longer story timeline for `matched` (a word weaveStory found), or
  // empty.
  std::string weaveTimeline(const std::string& matched) const;
  // Ranks `definition`'s senses for the passage and returns the Story,
  // Related and Riff sections for it (`storyWord`: the story-graph word).
  DictionaryLookup::LookupSections addConceptNotes(std::string& definition, bool html, const std::string& story,
                                                   std::string storyLong, const std::string& subject,
                                                   const std::string& storyWord);

  std::unique_ptr<ReaderWordPage> page;
  const int marginLeft;
  const int marginTop;
  const bool direct = false;
  bool directLookupPending = false;
  int fontId = 0;
  int lineHeight = 0;

  std::vector<WordBox> words;
  int selected = 0;
  uint16_t rowCount = 0;
  uint32_t readingPosition = 0;
  uint32_t storyCutoff = 0;
  bool hasReadingPosition = false;
  std::string bookTitle;
  unsigned long lastHorizontalMoveTime = 0;

  // Every installed dictionary is a lookup candidate (see DictionaryRouter);
  // the folder list is scanned once, at the first lookup.
  DictionaryRouter router;
  std::vector<std::string> installed;
  bool installedScanned = false;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint: the pixels under the current highlight
  // box, so a cursor move restores them and repaints only the two affected
  // boxes instead of re-running the full two-pass page render (which also
  // reloads every SD-font glyph on the page). snapshotIdx is the word whose
  // under-pixels are saved; -1 means the framebuffer no longer holds a clean
  // page (popup drawn, sub-activity shown) and the next render must be full.
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;
};
