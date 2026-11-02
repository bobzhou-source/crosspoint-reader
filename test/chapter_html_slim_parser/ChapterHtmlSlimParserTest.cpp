#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

namespace {

class ChapterHtmlSlimParserTest : public ::testing::TestWithParam<const char*> {
 protected:
  std::string filepath = "unused.xhtml";
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  ChapterHtmlSlimParser parser{nullptr,
                               filepath,
                               renderer,
                               0,
                               1.0f,
                               false,
                               0,
                               static_cast<uint16_t>(renderer.getScreenWidth()),
                               static_cast<uint16_t>(renderer.getScreenHeight()),
                               false,
                               false,
                               {},
                               true,
                               "",
                               "",
                               0,
                               {},
                               nullptr,
                               &cssParser};

  void SetUp() override { parser.currentTextBlock = std::make_unique<ParsedText>(false); }

  std::vector<std::unique_ptr<Page>> parseMarkup(const std::string& markup, bool paragraphSpacing = false) {
    const auto name = std::string("crosspoint-whitespace-") +
                      ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".xhtml";
    filepath = (std::filesystem::temp_directory_path() / name).string();
    {
      std::ofstream file(filepath);
      file << "<html><body>" << markup << "</body></html>";
    }
    std::vector<std::unique_ptr<Page>> pages;
    parser.extraParagraphSpacing = paragraphSpacing;
    parser.completePageFn = [&](std::unique_ptr<Page> page, auto, auto, auto) { pages.push_back(std::move(page)); };
    EXPECT_TRUE(parser.parseAndBuildPages());
    std::filesystem::remove(filepath);
    return pages;
  }

  static std::vector<const PageLine*> lines(const std::vector<std::unique_ptr<Page>>& pages) {
    std::vector<const PageLine*> result;
    for (const auto& page : pages) {
      for (const auto& element : page->elements) {
        if (element->getTag() == TAG_PageLine) result.push_back(static_cast<const PageLine*>(element.get()));
      }
    }
    return result;
  }
};

TEST_F(ChapterHtmlSlimParserTest, CollapsesWhitespaceAcrossInlineNodesWithoutInventingGaps) {
  const auto pages = parseMarkup("<p style=\"text-indent:0\"> \n Alpha <i> \t beta </i> gamma"
                                "<b>delta</b>, <span> \n </span> epsilon \r\n </p>");
  const auto rendered = lines(pages);
  ASSERT_EQ(rendered.size(), 1u);
  const auto* block = rendered[0]->getBlock();
  ASSERT_EQ(block->wordCount(), 6u);
  EXPECT_STREQ(block->wordText(0), "Alpha");
  EXPECT_STREQ(block->wordText(1), "beta");
  EXPECT_STREQ(block->wordText(2), "gamma");
  EXPECT_STREQ(block->wordText(3), "delta");
  EXPECT_STREQ(block->wordText(4), ",");
  EXPECT_STREQ(block->wordText(5), "epsilon");
  EXPECT_EQ(block->wordXpos(0), 0);
  EXPECT_EQ(block->wordXpos(1) - block->wordXpos(0), 44);
  EXPECT_EQ(block->wordXpos(2) - block->wordXpos(1), 36);
  EXPECT_EQ(block->wordXpos(3) - block->wordXpos(2), 40);
  EXPECT_EQ(block->wordXpos(4) - block->wordXpos(3), 40);
  EXPECT_EQ(block->wordXpos(5) - block->wordXpos(4), 12);
}

TEST_F(ChapterHtmlSlimParserTest, BreakContinuesParagraphWithoutIndentOrExtraParagraphSpacing) {
  const auto pages = parseMarkup("<p style=\"text-indent:2em;font-weight:bold\">Alpha<br/>\n  Beta<br/>Gamma</p>"
                                "<p style=\"text-indent:0\">Delta</p>", true);
  const auto rendered = lines(pages);
  ASSERT_EQ(rendered.size(), 4u);
  EXPECT_EQ(rendered[0]->getBlock()->wordXpos(0), 24);
  EXPECT_EQ(rendered[1]->getBlock()->wordXpos(0), 0);
  EXPECT_EQ(rendered[2]->getBlock()->wordXpos(0), 0);
  EXPECT_EQ(rendered[1]->yPos - rendered[0]->yPos, 16);
  EXPECT_EQ(rendered[2]->yPos - rendered[1]->yPos, 16);
  EXPECT_EQ(rendered[3]->yPos - rendered[2]->yPos, 24);
  EXPECT_EQ(rendered[1]->getBlock()->wordStyle(0) & EpdFontFamily::BOLD, EpdFontFamily::BOLD);
}

TEST_F(ChapterHtmlSlimParserTest, ConsecutiveBreaksPreserveExactlyOneBlankLine) {
  const auto pages = parseMarkup("<p style=\"text-indent:0\">Alpha<br/>\n<br/>\nBeta</p>", true);
  const auto rendered = lines(pages);
  ASSERT_EQ(rendered.size(), 2u);
  EXPECT_EQ(rendered[1]->yPos - rendered[0]->yPos, 32);
}

TEST_F(ChapterHtmlSlimParserTest, NonbreakingSpacesRemainExplicitAndNonbreaking) {
  const auto pages = parseMarkup("<p style=\"text-indent:0\">A&#160;&#160;B</p>");
  const auto rendered = lines(pages);
  ASSERT_EQ(rendered.size(), 1u);
  const auto* block = rendered[0]->getBlock();
  ASSERT_EQ(block->wordCount(), 4u);
  EXPECT_STREQ(block->wordText(1), " ");
  EXPECT_STREQ(block->wordText(2), " ");
  EXPECT_STREQ(block->wordText(3), "B");
}

TEST_F(ChapterHtmlSlimParserTest, RubyClosingTagsPreserveSourceWhitespace) {
  for (const std::string markup : {
           "<p style=\"text-indent:0\"><ruby>Alpha <rt>a</rt></ruby>Beta</p>",
           "<p style=\"text-indent:0\"><ruby>Alpha<rt>a</rt> </ruby>Beta</p>"}) {
    const auto pages = parseMarkup(markup);
    const auto rendered = lines(pages);
    ASSERT_EQ(rendered.size(), 1u);
    const auto* block = rendered[0]->getBlock();
    ASSERT_EQ(block->wordCount(), 2u);
    EXPECT_EQ(block->wordXpos(1) - block->wordXpos(0), 44);
  }
}

TEST_F(ChapterHtmlSlimParserTest, RubySurvivesPartialParagraphExtraction) {
  ParsedText text(false);
  text.addWord("a", EpdFontFamily::REGULAR);
  text.addWord("b", EpdFontFamily::REGULAR);
  text.addWord("c", EpdFontFamily::REGULAR);
  text.setRubyForWordAt(2, "c");
  size_t lines = 0;
  text.layoutAndExtractLines(
      renderer, 0, 20,
      [&](std::unique_ptr<TextBlock> line, auto) {
        ++lines;
        EXPECT_TRUE(line->getRubyTexts().empty());
      },
      false);
  EXPECT_EQ(lines, 1u);
  const size_t retainedWords = text.size();
  ASSERT_GT(retainedWords, 0u);
  ASSERT_LT(retainedWords, 3u);
  text.layoutAndExtractLines(renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, auto) {
    ++lines;
    ASSERT_EQ(line->getRubyTexts().size(), retainedWords);
    EXPECT_EQ(line->getRubyTexts().back(), "c");
    for (size_t i = 0; i + 1 < retainedWords; ++i) EXPECT_TRUE(line->getRubyTexts()[i].empty());
  });
  EXPECT_EQ(lines, 2u);
}

TEST_F(ChapterHtmlSlimParserTest, UnequalTableCellsAndRubySurvivePageBreaks) {
  parser.viewportWidth = 240;
  parser.viewportHeight = 32;
  parser.tableRowCells.reserve(2);
  std::multiset<std::string> expected;
  for (int column = 0; column < 2; ++column) {
    auto cell = std::make_unique<ParsedText>(false);
    for (int index = 0; index < (column == 0 ? 30 : 3); ++index) {
      const auto word = std::string(column == 0 ? "left" : "right") + std::to_string(index);
      expected.insert(word);
      cell->addWord(word, EpdFontFamily::REGULAR);
    }
    if (column == 0) cell->setRubyGroupAt(0, 2, "reading");
    parser.tableRowCells.push_back(std::move(cell));
  }
  std::multiset<std::string> actual;
  unsigned pages = 0;
  unsigned rubyLines = 0;
  auto inspect = [&](std::unique_ptr<Page> page, auto, auto, auto) {
    ++pages;
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      const auto& block = *line.getBlock();
      ASSERT_TRUE(block.valid());
      EXPECT_LE(element->yPos + 16 + block.getRubyShift(12), parser.viewportHeight);
      rubyLines += block.hasRuby();
      for (uint16_t word = 0; word < block.wordCount(); ++word) actual.insert(block.wordText(word));
    }
  };
  parser.completePageFn = inspect;
  parser.finishTableRow();
  ASSERT_NE(parser.currentPage, nullptr);
  inspect(std::move(parser.currentPage), 0, 0, 0);
  EXPECT_GT(pages, 2u);
  EXPECT_EQ(rubyLines, 1u);
  EXPECT_EQ(actual, expected);
  for (const auto& lines : parser.tableCellLines) EXPECT_TRUE(lines.empty());
}

TEST_F(ChapterHtmlSlimParserTest, PageImageDeserializeRejectsMissingImageBlock) {
  const auto path = std::filesystem::temp_directory_path() / "crosspoint-missing-image-cache.bin";
  {
    HalFile output;
    ASSERT_TRUE(output.open(path.c_str(), "wb"));
    const int16_t coordinates[] = {0, 0};
    output.write(coordinates, sizeof(coordinates));
  }
  HalFile input;
  ASSERT_TRUE(input.open(path.c_str(), "rb"));
  EXPECT_EQ(PageImage::deserialize(input), nullptr);
}

TEST_P(ChapterHtmlSlimParserTest, KeepsCssVerticalAlignAndInternalLinkMetadata) {
  const char* verticalAlign = GetParam();
  const char* expectedHref = "#note-target";
  const XML_Char* attributes[] = {"href", expectedHref, "style", verticalAlign, nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "a", attributes);
  const uint8_t linkId = parser.currentFootnoteLinkId;
  ASSERT_NE(linkId, 0u);
  ChapterHtmlSlimParser::characterData(&parser, "1", 1);
  ChapterHtmlSlimParser::endElement(&parser, "a");

  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  const auto style = parser.currentTextBlock->getWordStyleAt(0);
  const auto expectedStyle =
      std::string(verticalAlign).find("super") != std::string::npos ? EpdFontFamily::SUP : EpdFontFamily::SUB;
  EXPECT_NE(static_cast<uint8_t>(style) & static_cast<uint8_t>(expectedStyle), 0u);

  ASSERT_EQ(parser.pendingFootnotes.size(), 1u);
  const FootnoteEntry& footnote = parser.pendingFootnotes.front().second;
  EXPECT_STREQ(footnote.href, expectedHref);
  ASSERT_EQ(parser.currentTextBlock->wordLinkIds.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->wordLinkIds.front(), linkId);
  EXPECT_TRUE(parser.currentTextBlock->linkTargetMatches(linkId, expectedHref));
}

INSTANTIATE_TEST_SUITE_P(CssVerticalAlign, ChapterHtmlSlimParserTest,
                         ::testing::Values("vertical-align: super", "vertical-align: sub"));

TEST_F(ChapterHtmlSlimParserTest, ParagraphWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

TEST_F(ChapterHtmlSlimParserTest, HeaderWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "h1", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

TEST_F(ChapterHtmlSlimParserTest, SpanWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Before ", 7);
  ChapterHtmlSlimParser::startElement(&parser, "span", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::characterData(&parser, " After ", 7);

  ASSERT_EQ(parser.currentTextBlock->size(), 2);
  ASSERT_EQ(parser.currentTextBlock->wordAt(0), "Before");
  ASSERT_EQ(parser.currentTextBlock->wordAt(1), "After");
}

TEST_F(ChapterHtmlSlimParserTest, DivWithHiddenAttributeContentShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "div", attributes);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

}  // namespace

TEST(TextSpacingLayout, TrackingSeparatesCjkTokensAndScalesWordSpaces) {
  GfxRenderer renderer;
  for (bool hyphenation : {false, true}) {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    style.textIndentDefined = true;
    ParsedText text(false, hyphenation, false, style);
    text.addWord("一二三", EpdFontFamily::REGULAR);
    text.addWord("四五", EpdFontFamily::REGULAR);
    unsigned lines = 0;
    text.layoutAndExtractLines(
        renderer, 0, 200,
        [&](std::unique_ptr<TextBlock> line, auto) {
          ++lines;
          ASSERT_EQ(line->wordCount(), 5);
          EXPECT_EQ(line->wordXpos(0), 0);
          EXPECT_EQ(line->wordXpos(1), 7);  // 8 px glyph, -1 px tracking
          EXPECT_EQ(line->wordXpos(2), 14);
          EXPECT_EQ(line->wordXpos(3), 28);  // 8 px glyph plus 150% of a 4 px space, no tracking
          EXPECT_EQ(line->wordXpos(4), 35);
        },
        true, -1, 150);
    EXPECT_EQ(lines, 1u);
  }
  EXPECT_EQ(renderer.getTextAdvanceX(0, "ab", EpdFontFamily::REGULAR), 16);
  EXPECT_EQ(renderer.getSpaceWidth(0, EpdFontFamily::REGULAR), 4);
}

TEST(TextSpacingLayout, WordSpacingChangesWrapThreshold) {
  GfxRenderer renderer;
  for (uint8_t percent : {50, 100, 125, 200}) {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    style.textIndentDefined = true;
    ParsedText text(false, false, false, style);
    text.addWord("ab", EpdFontFamily::REGULAR);
    text.addWord("cd", EpdFontFamily::REGULAR);
    unsigned lines = 0;
    text.layoutAndExtractLines(renderer, 0, 36, [&](std::unique_ptr<TextBlock>, auto) { ++lines; }, true, 0, percent);
    EXPECT_EQ(lines, percent > 100 ? 2u : 1u);  // 16 + 16 + scaled 4 px space
  }
}

TEST(TextSpacingLayout, CachedPageRestoresSpacing) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textIndentDefined = true;
  ParsedText text(false, false, false, style);
  text.addWord("一二三", EpdFontFamily::REGULAR);
  text.addWord("四五", EpdFontFamily::REGULAR);
  const auto path = (std::filesystem::temp_directory_path() / "crosspoint-text-spacing.bin").string();
  unsigned lines = 0;
  text.layoutAndExtractLines(
      renderer, 0, 200,
      [&](std::unique_ptr<TextBlock> line, auto) {
        ++lines;
        Page page;
        page.elements.push_back(std::make_unique<PageLine>(std::move(line), 4, 12));
        const auto* original = static_cast<const PageLine&>(*page.elements[0]).getBlock();
        {
          HalFile file;
          ASSERT_TRUE(file.open(path.c_str(), "wb"));
          ASSERT_TRUE(page.serialize(file));
        }
        HalFile file;
        ASSERT_TRUE(file.open(path.c_str(), "rb"));
        auto cachedPage = Page::deserialize(file);
        ASSERT_NE(cachedPage, nullptr);
        ASSERT_EQ(cachedPage->elements.size(), 1);
        const auto* cached = static_cast<const PageLine&>(*cachedPage->elements[0]).getBlock();
        ASSERT_NE(cached, nullptr);
        EXPECT_EQ(cached->getBlockStyle().characterSpacing, -2);
        ASSERT_EQ(cached->wordCount(), 5);
        EXPECT_EQ(cached->wordXpos(3) - cached->wordXpos(2), 10);  // 8 + half-width space
        EXPECT_EQ(file.position(), file.size());
        ASSERT_EQ(cached->wordCount(), original->wordCount());
        for (uint16_t i = 0; i < original->wordCount(); ++i) EXPECT_EQ(cached->wordXpos(i), original->wordXpos(i));
      },
      true, -2, 50);
  EXPECT_EQ(lines, 1u);
  std::filesystem::remove(path);
}

TEST_F(ChapterHtmlSlimParserTest, ParserAppliesTextSpacingToParagraphs) {
  parser.setTextSpacing(-1, 150);
  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  const std::string text = "\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89 \xe5\x9b\x9b\xe4\xba\x94";  // 一二三 四五
  ChapterHtmlSlimParser::characterData(&parser, text.c_str(), static_cast<int>(text.size()));
  ChapterHtmlSlimParser::endElement(&parser, "p");
  parser.makePages();
  ASSERT_NE(parser.currentPage, nullptr);
  unsigned lines = 0;
  for (const auto& element : parser.currentPage->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& block = *static_cast<const PageLine&>(*element).getBlock();
    ++lines;
    ASSERT_EQ(block.wordCount(), 5);
    EXPECT_EQ(block.getBlockStyle().characterSpacing, -1);
    EXPECT_EQ(block.wordXpos(1) - block.wordXpos(0), 7);   // 8 px glyph, -1 px tracking
    EXPECT_EQ(block.wordXpos(3) - block.wordXpos(2), 14);  // glyph plus 150% of a 4 px space
  }
  EXPECT_EQ(lines, 1u);
}

TEST(KoreanLayout, HangulWordsStayWholeAndWrapAtSpaces) {
  GfxRenderer renderer;
  {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    style.textIndentDefined = true;
    ParsedText text(false, false, false, style);
    text.addWord("가나다", EpdFontFamily::REGULAR);
    text.addWord("라마", EpdFontFamily::REGULAR);
    text.addWord("3개를", EpdFontFamily::REGULAR);
    text.addWord("iPhone을", EpdFontFamily::REGULAR);
    std::vector<std::vector<std::string>> lines;
    text.layoutAndExtractLines(renderer, 0, 60, [&](std::unique_ptr<TextBlock> line, auto) {
      auto& words = lines.emplace_back();
      for (uint16_t i = 0; i < line->wordCount(); ++i) words.emplace_back(line->wordText(i));
    });
    // 가나다 라마 is 24 + 4 + 16 px; adding 3개를 would need 72 px, and no break exists inside it.
    const std::vector<std::vector<std::string>> expected{{"가나다", "라마"}, {"3개를"}, {"iPhone을"}};
    EXPECT_EQ(lines, expected);
  }
}

TEST(KoreanLayout, JustifiedHangulStretchesOnlyWordSpaces) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  ParsedText text(false, false, false, style);
  for (const char* word : {"가나", "다라", "마바", "사아"}) text.addWord(word, EpdFontFamily::REGULAR);
  unsigned lines = 0;
  text.layoutAndExtractLines(renderer, 0, 60, [&](std::unique_ptr<TextBlock> line, auto) {
    if (lines++ != 0) return;
    // 3 x 16 px words + 2 x 4 px spaces leave 4 px, split across the two spaces only.
    ASSERT_EQ(line->wordCount(), 3);
    EXPECT_EQ(line->wordXpos(0), 0);
    EXPECT_EQ(line->wordXpos(1), 22);
    EXPECT_EQ(line->wordXpos(2), 44);
  });
  EXPECT_EQ(lines, 2u);
}

TEST(KoreanLayout, HangulGluedAcrossInlineStyleIsUnbreakable) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  ParsedText text(false, false, false, style);
  text.addWord("가나", EpdFontFamily::REGULAR);
  text.addWord("한국", EpdFontFamily::REGULAR);
  text.addWord("어", EpdFontFamily::BOLD, false, /*attachToPrevious=*/true);
  std::vector<std::vector<std::string>> lines;
  text.layoutAndExtractLines(renderer, 0, 40, [&](std::unique_ptr<TextBlock> line, auto) {
    auto& words = lines.emplace_back();
    for (uint16_t i = 0; i < line->wordCount(); ++i) words.emplace_back(line->wordText(i));
  });
  // 가나 한국 fits in 36 px, but 어 is glued to 한국, so the whole word moves down.
  const std::vector<std::vector<std::string>> expected{{"가나"}, {"한국", "어"}};
  EXPECT_EQ(lines, expected);
}

namespace {

// One 4-character word per line: the stub font is 8 px per glyph, so "a000 a001" (68 px) never fits in 40 px.
std::string paragraph(char prefix, int lineCount, const char* attributes = "") {
  std::string markup = std::string("<p style=\"text-indent:0;margin:0\"") + attributes + ">";
  for (int i = 0; i < lineCount; ++i) {
    char word[8];
    snprintf(word, sizeof(word), "%c%03d", prefix, i);
    if (i > 0) markup += ' ';
    markup += word;
  }
  return markup + "</p>";
}

// Each page as the first letter of every line, e.g. {"aaaa", "bbb"}.
std::vector<std::string> pageSignatures(const std::vector<std::unique_ptr<Page>>& pages) {
  std::vector<std::string> signatures;
  for (const auto& page : pages) {
    std::string signature;
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      signature += static_cast<const PageLine*>(element.get())->getBlock()->wordText(0)[0];
    }
    signatures.push_back(signature);
  }
  return signatures;
}

}  // namespace

TEST_F(ChapterHtmlSlimParserTest, OrphanFirstLineMovesWholeShortParagraphToNextPage) {
  parser.viewportWidth = 40;
  parser.viewportHeight = 80;  // five 16 px lines
  const auto pages = parseMarkup(paragraph('a', 4) + paragraph('b', 3, " id=\"next\""));
  const std::vector<std::string> expected{"aaaa", "bbb"};
  EXPECT_EQ(pageSignatures(pages), expected);
  ASSERT_EQ(parser.anchorData.size(), 1u);
  EXPECT_EQ(parser.anchorData[0].first, "next");
  EXPECT_EQ(parser.anchorData[0].second, 1u);
}

TEST_F(ChapterHtmlSlimParserTest, ShortParagraphIsNeverSplitIntoOrphanAndWidow) {
  parser.viewportWidth = 40;
  parser.viewportHeight = 80;
  const auto pages = parseMarkup(paragraph('a', 3) + paragraph('b', 3));
  const std::vector<std::string> expected{"aaa", "bbb"};
  EXPECT_EQ(pageSignatures(pages), expected);
}

TEST_F(ChapterHtmlSlimParserTest, WidowLinePullsAnotherLineToNextPage) {
  parser.viewportWidth = 40;
  parser.viewportHeight = 80;
  const auto pages = parseMarkup(paragraph('a', 2) + paragraph('b', 4));
  const std::vector<std::string> expected{"aabb", "bb"};
  EXPECT_EQ(pageSignatures(pages), expected);
  const auto rendered = lines(pages);
  ASSERT_EQ(rendered.size(), 6u);
  EXPECT_STREQ(rendered[3]->getBlock()->wordText(0), "b001");
  EXPECT_STREQ(rendered[4]->getBlock()->wordText(0), "b002");
  EXPECT_EQ(rendered[4]->yPos, 0);
}

TEST_F(ChapterHtmlSlimParserTest, ParagraphThatFillsThePageExactlyIsUnchanged) {
  parser.viewportWidth = 40;
  parser.viewportHeight = 80;
  const auto pages = parseMarkup(paragraph('a', 1) + paragraph('b', 4) + paragraph('c', 2));
  const std::vector<std::string> expected{"abbbb", "cc"};
  EXPECT_EQ(pageSignatures(pages), expected);
}

TEST_F(ChapterHtmlSlimParserTest, OrphanAndWidowControlHoldAcrossSoftFlushedParagraph) {
  parser.viewportWidth = 40;
  parser.viewportHeight = 80;
  // 801 words exceeds the soft-flush threshold, so the paragraph is laid out in several batches.
  const auto pages = parseMarkup(paragraph('a', 4) + paragraph('b', 801));
  const auto signatures = pageSignatures(pages);
  ASSERT_GE(signatures.size(), 3u);
  EXPECT_EQ(signatures.front(), "aaaa");
  EXPECT_EQ(signatures[signatures.size() - 2], "bbbb");
  EXPECT_EQ(signatures.back(), "bb");
  const auto rendered = lines(pages);
  ASSERT_EQ(rendered.size(), 805u);
  for (int i = 0; i < 801; ++i) {
    char word[8];
    snprintf(word, sizeof(word), "b%03d", i);
    ASSERT_STREQ(rendered[4 + i]->getBlock()->wordText(0), word);
  }
}

TEST_F(ChapterHtmlSlimParserTest, HeadingMovesToNextPageWithItsParagraph) {
  parser.viewportWidth = 40;
  parser.viewportHeight = 80;
  const auto pages = parseMarkup(paragraph('a', 3) + "<h2 id=\"h\" style=\"text-indent:0;margin:0\">Head</h2>" +
                                 paragraph('b', 3));
  const std::vector<std::string> expected{"aaa", "Hbbb"};
  EXPECT_EQ(pageSignatures(pages), expected);
  ASSERT_EQ(pages.size(), 2u);
  const auto rendered = lines(pages);
  ASSERT_EQ(rendered.size(), 7u);
  EXPECT_EQ(rendered[3]->yPos, 0);
  EXPECT_EQ(rendered[4]->yPos, 16);
  ASSERT_EQ(parser.anchorData.size(), 1u);
  EXPECT_EQ(parser.anchorData[0].second, 1u);
}

TEST_F(ChapterHtmlSlimParserTest, OversizedParagraphStillMakesProgressOnEmptyPages) {
  parser.viewportWidth = 40;
  parser.viewportHeight = 16;  // one line per page: orphan/widow control cannot be satisfied
  const auto pages = parseMarkup(paragraph('a', 3) + paragraph('b', 1));
  const std::vector<std::string> expected{"a", "a", "a", "b"};
  EXPECT_EQ(pageSignatures(pages), expected);
}

TEST_F(ChapterHtmlSlimParserTest, FloatedDropCapJoinsTheRestOfItsWord) {
  const auto pages = parseMarkup("<div style=\"text-indent:0\"><span style=\"float:left;font-size:3em\">M</span>\n"
                                 "ost people</div>");
  const auto rendered = lines(pages);
  ASSERT_EQ(rendered.size(), 1u);
  const auto* block = rendered[0]->getBlock();
  ASSERT_EQ(block->wordCount(), 2u);
  EXPECT_STREQ(block->wordText(0), "Most");
  EXPECT_STREQ(block->wordText(1), "people");
}

TEST_F(ChapterHtmlSlimParserTest, StyledFloatedDropCapStaysAttachedWithoutASpace) {
  const auto pages = parseMarkup("<div style=\"text-indent:0\"><span style=\"float:left;font-weight:bold\">M</span>"
                                 "\n  ost people</div>");
  const auto rendered = lines(pages);
  ASSERT_EQ(rendered.size(), 1u);
  const auto* block = rendered[0]->getBlock();
  ASSERT_EQ(block->wordCount(), 3u);
  EXPECT_STREQ(block->wordText(0), "M");
  EXPECT_STREQ(block->wordText(1), "ost");
  EXPECT_EQ(block->wordStyle(0) & EpdFontFamily::BOLD, EpdFontFamily::BOLD);
  EXPECT_EQ(block->wordXpos(1) - block->wordXpos(0), 8);
  EXPECT_EQ(block->wordXpos(2) - block->wordXpos(1), 28);
}

TEST_F(ChapterHtmlSlimParserTest, InFlowInlineElementsKeepTheirSourceSpace) {
  const auto pages = parseMarkup("<div style=\"text-indent:0\"><span>M</span>\nost</div>"
                                 "<div style=\"text-indent:0\">A <span style=\"float:left\">x</span>\n y</div>");
  const auto rendered = lines(pages);
  ASSERT_EQ(rendered.size(), 2u);
  const auto* first = rendered[0]->getBlock();
  ASSERT_EQ(first->wordCount(), 2u);
  EXPECT_STREQ(first->wordText(0), "M");
  EXPECT_STREQ(first->wordText(1), "ost");
  EXPECT_EQ(first->wordXpos(1) - first->wordXpos(0), 12);
  const auto* second = rendered[1]->getBlock();
  ASSERT_EQ(second->wordCount(), 3u);
  EXPECT_STREQ(second->wordText(1), "x");
  EXPECT_STREQ(second->wordText(2), "y");
}
