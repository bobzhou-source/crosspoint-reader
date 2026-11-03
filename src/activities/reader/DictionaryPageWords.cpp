#include "DictionaryPageWords.h"

#include <Epub/Page.h>
#include <GfxRenderer.h>

#include <string>

namespace DictionaryPageWords {

void extract(GfxRenderer& renderer, const Page& page, const int fontId, const int marginLeft, const int marginTop,
             std::vector<WordBox>& words, uint16_t& rowCount) {
  words.clear();
  words.reserve(128);
  rowCount = 0;

  // Single walk: collect the selectable words while accumulating their text
  // and styles (~2KB transient string, freed on return). Widths are measured
  // afterwards: merging the page's codepoints into the SD font's persistent
  // advance table first keeps getTextAdvanceX on the in-RAM path instead of
  // loading glyphs from SD one overflow slot at a time.
  std::string pageText;
  pageText.reserve(2048);
  uint8_t styleMask = 0;
  uint16_t run = 0;
  const int ascender = renderer.getFontAscenderSize(fontId);

  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto* block = line->getBlock();
    if (!block || !block->valid()) continue;

    bool rowHasWords = false;
    ++run;  // A rendered line is a bounded run; never cross paragraph/line gaps.
    const int y = line->yPos + marginTop + block->getRubyShift(ascender);
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const char* text = block->wordText(i);
      const auto style = block->wordStyle(i);
      rowHasWords |= appendToken(words, text, line->xPos + block->wordXpos(i) + marginLeft, y, rowCount, style, run);
      pageText.append(text);
      pageText.push_back(' ');
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(style) & 0x03));
    }
    if (rowHasWords) rowCount++;
  }

  if (styleMask == 0) styleMask = 0x01;  // REGULAR
  renderer.ensureSdCardFontReady(fontId, pageText.c_str(), styleMask);
  const auto advance = [&renderer, fontId](const char* text, const EpdFontFamily::Style style) {
    return renderer.getTextAdvanceX(fontId, text, style);
  };
  size_t wordIndex = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* block = static_cast<const PageLine*>(element.get())->getBlock();
    if (!block || !block->valid()) continue;
    for (uint16_t i = 0; i < block->wordCount() && wordIndex < words.size(); ++i) {
      measureToken(words, wordIndex, block->wordText(i), block->wordStyle(i), advance);
    }
  }
}

}  // namespace DictionaryPageWords
