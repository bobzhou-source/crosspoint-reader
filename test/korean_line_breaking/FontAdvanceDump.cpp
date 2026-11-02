// Actual TtfEpdFont/FreeInkFont/FreeType metrics, not the layout test renderer.
// Usage: FontAdvanceDump FONT.ttf POINT_SIZE
// stdin: STYLE TRACKING WORD_SPACING FOCUS_BOUNDARY FOCUS_SUFFIX_X "UTF8_WORD"
// stdout: one rendered advance per input record. Sources are opened read-only.
#include <BidiUtils.h>
#include <EpdFontFamily.h>
#include <TtfEpdFont.h>
#include <Utf8.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

namespace {
unsigned long streamRead(void* context, unsigned long offset, unsigned char* buffer, unsigned long count) {
  const ssize_t n = pread(*static_cast<int*>(context), buffer, count, static_cast<off_t>(offset));
  return n < 0 ? 0 : static_cast<unsigned long>(n);
}

int advance(const EpdFontFamily& family, const char* text, EpdFontFamily::Style style, int tracking) {
  std::string visual;
  if (BidiUtils::applyBidiVisual(text, visual) && !visual.empty()) text = visual.c_str();
  int width = 0;
  int previousAdvance = 0;
  uint32_t previous = 0;
  while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
    if (BidiUtils::isTransparentMark(cp) || utf8IsCombiningMark(cp)) continue;
    cp = family.applyLigatures(cp, text, style);
    if (previous) {
      const auto isSpace = [](uint32_t value) { return value == ' ' || value == 0xA0 || value == 0x3000; };
      width += fp4::toPixel(previousAdvance + family.getKerning(previous, cp, style));
      if (!isSpace(previous) && !isSpace(cp)) width += tracking;
    }
    const auto* glyph = family.getGlyph(cp, style);
    previousAdvance = glyph ? glyph->advanceX : 0;
    if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) previousAdvance = (previousAdvance + 1) / 2;
    previous = cp;
  }
  return width + fp4::toPixel(previousAdvance);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const int pointSize = std::atoi(argv[2]);
  if (pointSize < 1 || pointSize > 256) return 2;
  int fd = open(argv[1], O_RDONLY);
  if (fd < 0) return 2;
  const off_t size = lseek(fd, 0, SEEK_END);
  if (size <= 0) {
    close(fd);
    return 2;
  }
  int status = 0;
  {
    TtfEpdFont font;
    font.addStreamSource(TtfEpdFont::Regular, streamRead, &fd, static_cast<unsigned long>(size));
    if (!font.load(static_cast<uint16_t>(pointSize), true, 1024 * 1024, 4096)) {
      close(fd);
      return 3;
    }
    const auto family = font.family();
    int style, tracking, wordSpacing, boundary, suffixX;
    std::string word;
    while (std::cin >> style >> tracking >> wordSpacing >> boundary >> suffixX >> std::quoted(word)) {
      if (style < 0 || style > 255 || tracking < -128 || tracking > 127 || boundary < 0 ||
          static_cast<size_t>(boundary) > word.size()) {
        status = 2;
        break;
      }
      const auto fontStyle = static_cast<EpdFontFamily::Style>(style);
      if (word == " ") {
        const auto* glyph = family.getGlyph(' ', fontStyle);
        const int space = glyph ? fp4::toPixel(glyph->advanceX) : 0;
        std::cout << (space * wordSpacing + 50) / 100 << '\n';
      } else {
        std::cout << (boundary ? suffixX : 0) + advance(family, word.c_str() + boundary, fontStyle, tracking) << '\n';
      }
    }
    if (!std::cin.eof() && status == 0) status = 2;
  }
  close(fd);
  return status;
}
