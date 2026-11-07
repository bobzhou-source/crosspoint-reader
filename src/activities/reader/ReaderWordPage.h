#pragma once

#include <Epub/Page.h>
#include <EpdFontFamily.h>

#include <memory>
#include <string>
#include <vector>

#include "DictionaryPageWords.h"

class GfxRenderer;

// A reader page the dictionary picker can redraw and scan for selectable
// words. EPUB pages are loaded from a Section; TXT pages are the display
// lines the reader laid out, captured with their draw positions.
class ReaderWordPage {
 public:
  virtual ~ReaderWordPage() = default;

  virtual void render(GfxRenderer& renderer, int fontId, int marginLeft, int marginTop) const = 0;
  virtual void extractWords(GfxRenderer& renderer, int fontId, int marginLeft, int marginTop,
                            std::vector<DictionaryPageWords::WordBox>& words, uint16_t& rowCount) = 0;
};

class EpubReaderWordPage final : public ReaderWordPage {
 public:
  explicit EpubReaderWordPage(std::unique_ptr<Page> page) : page(std::move(page)) {}

  void render(GfxRenderer& renderer, int fontId, int marginLeft, int marginTop) const override;
  void extractWords(GfxRenderer& renderer, int fontId, int marginLeft, int marginTop,
                    std::vector<DictionaryPageWords::WordBox>& words, uint16_t& rowCount) override;

 private:
  std::unique_ptr<Page> page;
};

// Lines are stored with their final screen positions, so render() ignores the
// margins it is handed. Token storage is kept here because WordBox::text
// points into it.
class TxtReaderWordPage final : public ReaderWordPage {
 public:
  struct Line {
    std::string text;
    int x;
    int y;
  };

  explicit TxtReaderWordPage(std::vector<Line> lines) : lines(std::move(lines)) {}

  void render(GfxRenderer& renderer, int fontId, int marginLeft, int marginTop) const override;
  void extractWords(GfxRenderer& renderer, int fontId, int marginLeft, int marginTop,
                    std::vector<DictionaryPageWords::WordBox>& words, uint16_t& rowCount) override;

 private:
  std::vector<Line> lines;
  std::vector<std::string> tokenStorage;
};
