#include <gtest/gtest.h>

#include <TxtChapters.h>

#include <string>

namespace {

bool heading(const std::string& line) { return TxtChapters::isHeading(line.data(), line.size()); }

std::string trimmed(const std::string& line) {
  size_t length = 0;
  const char* start = TxtChapters::trimHeading(line.data(), line.size(), length);
  return std::string(start, length);
}

}  // namespace

TEST(TxtChapters, ChineseHeadingsAreDetected) {
  for (const char* line : {"第一章", "第一章 风起云涌", "　　第十二章：决战", "第123章 标题", "第１２章", "第十二回",
                           "第三卷 江湖", "第2节", "第一部", "第一篇", "第一集", "第一千零一章 归来", "第二十回　夜宴",
                           "第一章\r", "  第五章  "}) {
    EXPECT_TRUE(heading(line)) << line;
  }
}

TEST(TxtChapters, NamedSectionsAreDetected) {
  for (const char* line : {"序章", "序", "序言", "楔子", "引子", "尾声", "尾聲", "终章", "番外一 重逢", "番外篇",
                           "后记", "後記", "完本感言", "外传 少年游"}) {
    EXPECT_TRUE(heading(line)) << line;
  }
}

TEST(TxtChapters, EnglishHeadingsAreDetected) {
  for (const char* line : {"Chapter 3", "CHAPTER XII", "Chapter Twenty-One", "Chapter 3: The Return", "chapter 7",
                           "Prologue", "Epilogue: Home"}) {
    EXPECT_TRUE(heading(line)) << line;
  }
  EXPECT_TRUE(heading("Chapter 1 " + std::string(30, 'x')));
}

TEST(TxtChapters, ProseMentioningChaptersIsNotAHeading) {
  for (const char* line : {"第一章就这么写完了。", "他翻开了第一章，", "第一次见面", "第一章，", "第十二回合", "第一部分",
                           "第三节课", "序列号", "番茄", "Chapters of life", "Chapter 3 was the dull one.",
                           "Chapter 12345", "Chapter", "Chapterhouse", "Prologues", "他说：「第一章。」"}) {
    EXPECT_FALSE(heading(line)) << line;
  }
}

TEST(TxtChapters, BlankAndOverlongLinesAreNotHeadings) {
  EXPECT_FALSE(heading(""));
  EXPECT_FALSE(heading("   "));
  EXPECT_FALSE(heading("　　"));
  EXPECT_FALSE(heading("第章"));
  EXPECT_FALSE(heading("第一"));
  EXPECT_FALSE(heading("第" + std::string(13, '1') + "章"));
  EXPECT_FALSE(heading("Chapter 1 " + std::string(31, 'x')));  // more than 40 code points
  std::string longTitle = "第一章 ";
  while (longTitle.size() <= TxtChapters::MAX_HEADING_BYTES) longTitle += "长";
  EXPECT_FALSE(heading(longTitle));
}

TEST(TxtChapters, HeadingTextIsTrimmed) {
  EXPECT_EQ(trimmed("　　第一章 风起\r"), "第一章 风起");
  EXPECT_EQ(trimmed("\xEF\xBB\xBF" "第一章"), "第一章");
  EXPECT_EQ(trimmed(" \tChapter 3\t "), "Chapter 3");
  EXPECT_EQ(trimmed("  \t"), "");
  EXPECT_EQ(trimmed(""), "");
}
