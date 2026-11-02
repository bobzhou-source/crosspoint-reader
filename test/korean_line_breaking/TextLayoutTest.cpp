#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {
struct Token {
  const char* text;
  bool attached = false;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
};
using Lines = std::vector<std::unique_ptr<TextBlock>>;

Lines layout(const std::vector<Token>& tokens, int width = 160, bool hyphenation = false, bool focus = false,
             CssTextAlign alignment = CssTextAlign::Justify, int8_t tracking = 2, bool rtl = false) {
  Hyphenator::setPreferredLanguage("en");
  BlockStyle style;
  style.alignment = alignment;
  style.textIndentDefined = true;
  style.directionDefined = true;
  style.isRtl = rtl;
  ParsedText text(false, hyphenation, focus, style);
  for (const auto& token : tokens) text.addWord(token.text, token.style, false, token.attached);
  Lines lines;
  text.layoutAndExtractLines(GfxRenderer{}, 0, width,
                             [&](std::unique_ptr<TextBlock> line, auto) { lines.push_back(std::move(line)); },
                             true, tracking);
  return lines;
}

std::string joined(const TextBlock& line) {
  std::string result;
  for (uint16_t i = 0; i < line.wordCount(); ++i) result += line.wordText(i);
  return result;
}

int gap(const TextBlock& line, uint16_t before) {
  const int advance = std::string_view(line.wordText(before)) == " "
                          ? 4
                          : GfxRenderer{}.getTextAdvanceX(0, line.wordText(before), line.wordStyle(before),
                                                        line.getBlockStyle().characterSpacing);
  return line.wordXpos(before + 1) - line.wordXpos(before) - advance;
}
}  // namespace

class TextLayout : public testing::TestWithParam<bool> {};
INSTANTIATE_TEST_SUITE_P(Breakers, TextLayout, testing::Bool());

TEST_P(TextLayout, OrdinarySpaceTokensCollapseWithoutDoubleMeasurement) {
  const auto lines = layout({{"one"}, {" "}, {" "}, {"two", true}}, 160, GetParam());
  ASSERT_EQ(lines.size(), 1u);
  ASSERT_EQ(lines[0]->wordCount(), 2);
  EXPECT_STREQ(lines[0]->wordText(0), "one");
  EXPECT_STREQ(lines[0]->wordText(1), "two");
  EXPECT_EQ(gap(*lines[0], 0), 4);
}

TEST_P(TextLayout, OrdinaryEdgeSpacesNeverMeasureOrDraw) {
  const auto lines = layout({{" "}, {"one"}, {" "}, {"two"}, {" "}}, 28, GetParam(), false, CssTextAlign::Left);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(joined(*lines[0]), "one");
  EXPECT_EQ(joined(*lines[1]), "two");
  for (const auto& line : lines) EXPECT_EQ(line->wordXpos(0), 0);
}

TEST_P(TextLayout, NbspWidthAndRepeatedNbspArePreserved) {
  const auto lines = layout({{" " , true}, {"one", true}, {" ", true}, {" ", true}, {"two", true}, {" ", true}},
                            160, GetParam(), false, CssTextAlign::Left);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(joined(*lines[0]), " one  two ");
  ASSERT_EQ(lines[0]->wordCount(), 6);
  EXPECT_EQ(lines[0]->wordXpos(1), 4);
  EXPECT_EQ(lines[0]->wordXpos(4) - lines[0]->wordXpos(2), 8);
}

TEST_P(TextLayout, NoOrphanNbspWhenUnbreakableGroupIsWiderThanViewport) {
  const auto lines = layout({{"AB"}, {" ", true}, {"CD", true}, {"E"}}, 20, GetParam(), false, CssTextAlign::Left, 0);
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(joined(*lines[0]), "AB CD");
  for (const auto& line : lines) {
    EXPECT_NE(std::string(line->wordText(0)), " ");
    EXPECT_NE(std::string(line->wordText(line->wordCount() - 1)), " ");
  }
}

TEST_P(TextLayout, JustificationIsCappedAndFinalLineIsNatural) {
  const auto lines = layout({{"aa"}, {"bb"}, {"1234567890"}}, 90, GetParam(), false, CssTextAlign::Justify, 0);
  ASSERT_GE(lines.size(), 2u);
  for (const auto& line : lines) {
    for (uint16_t i = 1; i < line->wordCount(); ++i) EXPECT_LE(gap(*line, i - 1), 8);
  }
  const auto finalLine = layout({{"aa"}, {"bb"}}, 300, GetParam());
  ASSERT_EQ(finalLine.size(), 1u);
  EXPECT_EQ(gap(*finalLine[0], 0), 4);
}

TEST_P(TextLayout, SourceCjkAndMixedScriptSpacesRemainSingleNaturalSpaces) {
  const auto title = layout({{"第79章"}, {"夺宝"}}, 300, GetParam());
  ASSERT_EQ(title.size(), 1u);
  // CJK tokenization produces 第 / 79 / 章 / 夺 / 宝; only the source separator is a space.
  ASSERT_EQ(title[0]->wordCount(), 5);
  EXPECT_EQ(gap(*title[0], 2), 4);
  EXPECT_EQ(gap(*title[0], 0), 2);
  const auto mixed = layout({{"中文"}, {"Latin"}, {"文字"}}, 300, GetParam());
  ASSERT_EQ(mixed.size(), 1u);
  EXPECT_EQ(gap(*mixed[0], 1), 4);
  EXPECT_EQ(gap(*mixed[0], 2), 4);
}

TEST_P(TextLayout, InlineBoundariesPreserveSourceSeparationAndAttachment) {
  const auto lines = layout({{"one"}, {"two", false, EpdFontFamily::ITALIC}, {"three", false},
                             {"four", true, EpdFontFamily::BOLD}}, 300, GetParam());
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(gap(*lines[0], 0), 4);
  EXPECT_EQ(gap(*lines[0], 1), 4);
  EXPECT_EQ(gap(*lines[0], 2), 2);
}

TEST_P(TextLayout, KinsokuAcrossSeparateStyledTokens) {
  for (const char* close : {"。", "，", "、", "！", "？", "）", "」", "』"}) {
    const auto lines = layout({{"甲乙"}, {close, true, EpdFontFamily::ITALIC}, {"丙丁", true}}, 16,
                              GetParam(), false, CssTextAlign::Left, 0);
    for (const auto& line : lines) EXPECT_NE(std::string(line->wordText(0)), close) << close;
  }
  for (const char* open : {"（", "「", "『"}) {
    const auto lines = layout({{"甲乙"}, {open, true}, {"丙丁", true}}, 16, GetParam(), false, CssTextAlign::Left, 0);
    for (const auto& line : lines) EXPECT_NE(std::string(line->wordText(line->wordCount() - 1)), open) << open;
  }
}

TEST_P(TextLayout, OversizedKinsokuGroupStaysTogetherRatherThanOrphaningPunctuation) {
  const auto lines = layout({{"甲"}, {"（", true}, {"乙", true}, {"）", true}, {"丙", true}}, 16,
                            GetParam(), false, CssTextAlign::Left, 0);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(joined(*lines[0]), "甲");
  EXPECT_EQ(joined(*lines[1]), "（乙）");
  EXPECT_EQ(joined(*lines[2]), "丙");
}

TEST_P(TextLayout, SoftHyphensAreInvisibleWithAndWithoutFocus) {
  for (bool focus : {false, true}) {
    const auto lines = layout({{"ab\xC2\xAD" "cdef"}, {"word"}}, 200, GetParam(), focus);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(joined(*lines[0]), "abcdefword");
    for (uint16_t i = 0; i < lines[0]->wordCount(); ++i) {
      EXPECT_GT(lines[0]->wordTextLen(i), 0);
      EXPECT_EQ(std::string(lines[0]->wordText(i)).find("\xC2\xAD"), std::string::npos);
      EXPECT_LT(lines[0]->focusBoundary(i), lines[0]->wordTextLen(i));
    }
  }
}

TEST_P(TextLayout, KinsokuAlsoAppliesAcrossSourceSpaces) {
  const auto lines = layout({{"甲乙"}, {"。"}, {"丙丁"}}, 16, GetParam(), false, CssTextAlign::Left, 0);
  for (const auto& line : lines) EXPECT_NE(std::string(line->wordText(0)), "。");
}

TEST_P(TextLayout, SoftHyphenRemovalPreservesFocusMetricsAndCjkTracking) {
  for (const char* source : {"a\xC2\xAD" "bcdef", "\xC2\xAD" "abcdef", "abcdef\xC2\xAD",
                              ".\xC2\xAD" "abcdef"}) {
    std::string clean(source);
    while (clean.find("\xC2\xAD") != std::string::npos) clean.erase(clean.find("\xC2\xAD"), 2);
    const auto actual = layout({{source}, {"end"}}, 300, GetParam(), true);
    const auto expected = layout({{clean.c_str()}, {"end"}}, 300, GetParam(), true);
    ASSERT_EQ(actual.size(), expected.size());
    ASSERT_EQ(actual[0]->wordCount(), expected[0]->wordCount());
    for (uint16_t i = 0; i < actual[0]->wordCount(); ++i) {
      EXPECT_STREQ(actual[0]->wordText(i), expected[0]->wordText(i));
      EXPECT_EQ(actual[0]->wordXpos(i), expected[0]->wordXpos(i));
      EXPECT_EQ(actual[0]->focusBoundary(i), expected[0]->focusBoundary(i));
      EXPECT_EQ(actual[0]->focusSuffixX(i), expected[0]->focusSuffixX(i));
    }
  }
  const auto cjk = layout({{"甲\xC2\xAD" "乙"}}, 300, GetParam());
  ASSERT_EQ(cjk.size(), 1u);
  ASSERT_EQ(cjk[0]->wordCount(), 2);
  EXPECT_EQ(gap(*cjk[0], 0), 2);
}

TEST(TextLayoutRtl, JustificationIsCappedAfterVisualReordering) {
  const auto lines = layout({{"אב"}, {"גד"}, {"1234567890"}}, 90, false, false, CssTextAlign::Justify, 0, true);
  ASSERT_GE(lines.size(), 2u);
  for (const auto& line : lines) {
    for (uint16_t i = 1; i < line->wordCount(); ++i) EXPECT_LE(gap(*line, i - 1), 8);
  }
}

TEST(TextLayoutHyphenation, DiscretionaryHyphenAppearsOnlyWhenBreaking) {
  for (bool focus : {false, true}) {
    const auto lines = layout({{"ab\xC2\xAD" "cdef"}}, 30, true, focus, CssTextAlign::Left, 0);
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(joined(*lines[0]), "ab-");
    for (const auto& line : lines) EXPECT_EQ(joined(*line).find("\xC2\xAD"), std::string::npos);
  }
}
