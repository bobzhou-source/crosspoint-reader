#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "DictionaryLookupSections.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/Dictionary.h"

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain
// text, or HTML too damaged to parse) is word-wrapped once on entry and each
// page renders spans of the original string, so no per-line copies are held.
// With concept-engine sections (Story, Related, Riff) it instead shows
// [All | Def | Story | Related | Riff] tabs and a [Compact | Longer] toggle
// over plain-text sections, each under a ruled header.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false,
                                        Dictionary* dictionary = nullptr, const char* run = "", uint8_t matchMask = 0)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(std::move(headword)),
        definition(std::move(definition)),
        htmlDefinition(htmlDefinition),
        dictionary(dictionary),
        matchMask(matchMask) {
    strncpy(cjkRun, run, sizeof(cjkRun) - 1);
  }

  // Story, Related and Riff texts to show beside the definition (which fills
  // the Def section itself). Call before the activity starts; without any of
  // them the plain viewer stays.
  void setSections(DictionaryLookup::LookupSections value) {
    sections = std::move(value);
    sectioned = sections.hasExtras();
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // One wrapped display line: a byte span of `definition`. Wrapping keeps
  // lines under the screen width, so uint16_t length is ample.
  struct Line {
    uint32_t start;
    uint16_t len;
  };

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int width;
    int height;
  };

  BodyArea bodyArea() const;
  // Height of the rows above the body: the CJK match bar, then the section
  // tabs and the length toggle.
  int controlsHeight() const;
  bool layoutHtmlPages();
  void wrapText();
  int measureSpan(int fontId, const char* text, size_t len) const;
  void drawBody(int fontId, int x, int startY, int width) const;
  void drawSectionControls(int x, int y, int width) const;
  void changeMatch(int direction);
  void layoutDefinition();
  // Sectioned view: composes the active tab at the current length into
  // `definition` and wraps and paginates it.
  void layoutSections();
  void selectSection(DictionaryLookup::Section section);
  void setLonger(bool value);
  // The tabs with text, in order (All first).
  size_t availableSections(DictionaryLookup::Section out[DictionaryLookup::SECTION_COUNT]) const;
  bool isHeaderLine(size_t line) const;
  int pageFirstLine(int page) const;
  int pageEndLine(int page) const;

  std::string headword;
  // Not const: onEnter() normalizes embedded NULs (StarDict multi-type
  // separators) to newlines so C-string APIs see the whole text.
  std::string definition;
  const bool htmlDefinition;
  Dictionary* dictionary;  // Parent word selector owns it throughout this activity.
  char cjkRun[CjkDictionary::RUN_BYTES] = {};
  uint8_t matchMask = 0;
  bool lookupFailed = false;
  StrId lookupError = StrId::STR_DICT_ERROR;
  // Styled path: reader-identical Pages laid out from the HTML definition.
  // Empty means the plain-text span path below is active.
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<Line> lines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  ButtonNavigator buttonNavigator;
  DictionaryLookup::LookupSections sections;
  bool sectioned = false;
  bool longer = false;
  DictionaryLookup::Section activeSection = DictionaryLookup::Section::All;
  // Byte offsets of the header lines in the composed `definition`.
  std::vector<uint32_t> headerOffsets;
  // First line of each page in the sectioned view.
  std::vector<uint32_t> pageStarts;
};
