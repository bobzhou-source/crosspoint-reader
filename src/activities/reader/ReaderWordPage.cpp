#include "ReaderWordPage.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <string>

void EpubReaderWordPage::render(GfxRenderer& renderer, const int fontId, const int marginLeft,
                                const int marginTop) const {
  if (page) page->render(renderer, fontId, marginLeft, marginTop);
}

void EpubReaderWordPage::extractWords(GfxRenderer& renderer, const int fontId, const int marginLeft,
                                      const int marginTop, std::vector<DictionaryPageWords::WordBox>& words,
                                      uint16_t& rowCount) {
  if (!page) {
    words.clear();
    rowCount = 0;
    return;
  }
  DictionaryPageWords::extract(renderer, *page, fontId, marginLeft, marginTop, words, rowCount);
}

void TxtReaderWordPage::render(GfxRenderer& renderer, const int fontId, const int marginLeft,
                               const int marginTop) const {
  (void)marginLeft;
  (void)marginTop;
  for (const auto& line : lines) {
    if (!line.text.empty()) renderer.drawText(fontId, line.x, line.y, line.text.c_str());
  }
}

void TxtReaderWordPage::extractWords(GfxRenderer& renderer, const int fontId, const int marginLeft,
                                     const int marginTop, std::vector<DictionaryPageWords::WordBox>& words,
                                     uint16_t& rowCount) {
  (void)marginLeft;
  (void)marginTop;
  std::vector<DictionaryPageWords::LineText> textLines;
  textLines.reserve(lines.size());
  std::string pageText;
  pageText.reserve(lines.size() * 40);
  for (const auto& line : lines) {
    textLines.push_back({line.text.c_str(), line.x, line.y, EpdFontFamily::REGULAR});
    pageText.append(line.text);
    pageText.push_back(' ');
  }
  // Merge the page's codepoints into an SD font's advance table first, as the
  // EPUB extractor does, so getTextAdvanceX stays on the in-RAM path.
  if (!pageText.empty()) renderer.ensureSdCardFontReady(fontId, pageText.c_str(), /*styleMask=*/0x01);
  // UTF-8 advance of a range: the TXT reader draws whole lines, so the token
  // offset must be measured from the line start the same way.
  const auto advance = [&renderer, fontId](const char* text, const size_t length) {
    if (length == 0) return 0;
    return renderer.getTextAdvanceX(fontId, std::string(text, length).c_str(), EpdFontFamily::REGULAR);
  };
  DictionaryPageWords::extractFromLines(textLines.data(), textLines.size(), tokenStorage, words, rowCount, advance);
}
