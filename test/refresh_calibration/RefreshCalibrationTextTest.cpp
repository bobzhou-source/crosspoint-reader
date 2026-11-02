#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "RefreshCalibrationText.h"

namespace rc = refresh_calibration;

namespace {

// Ten pixels per codepoint: a stand-in for the renderer's measurement.
int tenPerCodepoint(const void*, const char* text) {
  int width = 0;
  for (const char* p = text; *p; ++p) {
    if ((static_cast<unsigned char>(*p) & 0xC0) != 0x80) width += 10;
  }
  return width;
}

std::vector<std::string> lines(const char* corpus, const bool cjk, const int maxWidth, const int count,
                               std::vector<uint32_t>* offsets = nullptr, const size_t bufferSize = 192) {
  std::vector<std::string> out;
  std::vector<char> buffer(bufferSize);
  uint32_t offset = 0;
  for (int i = 0; i < count; ++i) {
    offset = rc::nextFixtureLine(corpus, strlen(corpus), offset, cjk, maxWidth, tenPerCodepoint, nullptr,
                                 buffer.data(), buffer.size());
    out.emplace_back(buffer.data());
    if (offsets) offsets->push_back(offset);
  }
  return out;
}

constexpr char kWords[] = "It is a truth universally acknowledged, that a single man in possession of a good fortune "
                          "must be in want of a wife. ";

}  // namespace

TEST(RefreshCalibrationText, LatinLinesFillTheWidthWithWholeWords) {
  std::vector<uint32_t> offsets;
  const auto got = lines(kWords, false, 300, 6, &offsets);
  std::string rebuilt;
  for (size_t i = 0; i < got.size(); ++i) {
    const auto& line = got[i];
    EXPECT_LE(tenPerCodepoint(nullptr, line.c_str()), 300) << line;
    EXPECT_NE(line.front(), ' ') << line;
    EXPECT_NE(line.back(), ' ') << line;
    if (!rebuilt.empty()) rebuilt += ' ';
    rebuilt += line;
    if (offsets[i] == 0) break;  // the corpus ended: its last line may be a single word
    // A dense page: a 30-character line holds several words, not one.
    EXPECT_NE(line.find(' '), std::string::npos) << line;
  }
  EXPECT_EQ(rebuilt + ' ', std::string(kWords));
}

TEST(RefreshCalibrationText, TheCorpusLoopsInsteadOfRunningDry) {
  std::vector<uint32_t> offsets;
  const auto got = lines("alpha beta gamma ", false, 60, 300, &offsets);
  int wraps = 0;
  for (size_t i = 0; i < got.size(); ++i) {
    ASSERT_FALSE(got[i].empty()) << "line " << i;
    if (offsets[i] == 0) ++wraps;
  }
  EXPECT_GE(wraps, 100);
  EXPECT_EQ(got[0], "alpha");
  EXPECT_EQ(got[3], "alpha");
}

TEST(RefreshCalibrationText, AWordWiderThanTheLineStillAdvances) {
  const auto got = lines("extraordinarily long words here ", false, 20, 8);
  EXPECT_EQ(got[0], "extraordinarily");
  EXPECT_EQ(got[1], "long");
  EXPECT_EQ(got[2], "words");
  EXPECT_EQ(got[3], "here");
  EXPECT_EQ(got[4], "extraordinarily");
}

TEST(RefreshCalibrationText, CjkLinesBreakOnWholeCodepoints) {
  const char* corpus = "晋太元中武陵人捕鱼为业缘溪行忘路之远近忽";  // 20 codepoints, 4 to a line
  const auto got = lines(corpus, true, 45, 5);
  std::string rebuilt;
  for (const auto& line : got) {
    EXPECT_EQ(line.size(), 4u * 3u) << line;
    rebuilt += line;
  }
  EXPECT_EQ(rebuilt, std::string(corpus));
}

TEST(RefreshCalibrationText, TheLineNeverOverrunsTheBuffer) {
  const auto got = lines("abcdefghijklmnop qr ", false, 1000, 4, nullptr, 8);
  for (const auto& line : got) {
    EXPECT_FALSE(line.empty());
    EXPECT_LE(line.size(), 7u);
  }
  EXPECT_EQ(got[0], "abcdefg");
}
