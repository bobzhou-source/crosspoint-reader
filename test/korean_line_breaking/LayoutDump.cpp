#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <Utf8.h>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

// Plain paragraph input, not an HTML parser: whitespace maps to the same addWord
// contract as ChapterHtmlSlimParser. Newlines are collapsible source whitespace.
// Stress fixture: forced justification and zero indent, not source HTML/CSS layout.
// Usage: LayoutDump [width=160] [tracking=2] [hyphenate=0] < paragraph.txt
int main(int argc, char** argv) {
  const int width = argc > 1 ? std::atoi(argv[1]) : 160;
  const int tracking = argc > 2 ? std::atoi(argv[2]) : 2;
  if (width <= 0 || width > 32767 || tracking < -128 || tracking > 127) return 2;
  const std::string input{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  ParsedText text(false, argc > 3 && std::atoi(argv[3]) != 0, false, style);
  const auto* start = reinterpret_cast<const unsigned char*>(input.c_str());
  const auto* ptr = start;
  const auto* wordStart = ptr;
  bool attach = false;
  const auto flush = [&](const unsigned char* end) {
    if (end > wordStart) {
      text.addWord(std::string(reinterpret_cast<const char*>(wordStart), end - wordStart), EpdFontFamily::REGULAR,
                   false, attach);
    }
  };
  while (*ptr) {
    const auto* before = ptr;
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == '\f') {
      flush(before);
      wordStart = ptr;
      attach = false;
    } else if (cp == 0xA0 || cp == 0x202F) {
      flush(before);
      text.addWord(" ", EpdFontFamily::REGULAR, false, true);
      wordStart = ptr;
      attach = true;
    }
  }
  flush(ptr);

  GfxRenderer renderer;
  std::cout << "{\"metrics\":\"host: glyph=8, space=4; not device font metrics\",\"width\":" << width
            << ",\"tracking\":" << tracking
            << ",\"alignment\":\"forced-justify\",\"source_html_styles\":false,\"lines\":[";
  size_t count = 0;
  text.layoutAndExtractLines(renderer, 0, static_cast<uint16_t>(width), [&](std::unique_ptr<TextBlock> line, auto) {
    if (count++) std::cout << ',';
    std::cout << "{\"words\":[";
    for (uint16_t i = 0; i < line->wordCount(); ++i) {
      if (i) std::cout << ',';
      std::cout << std::quoted(line->wordText(i));
    }
    std::cout << "],\"x\":[";
    for (uint16_t i = 0; i < line->wordCount(); ++i) {
      if (i) std::cout << ',';
      std::cout << line->wordXpos(i);
    }
    std::cout << "],\"gaps\":[";
    for (uint16_t i = 1; i < line->wordCount(); ++i) {
      if (i > 1) std::cout << ',';
      const int advance = std::string_view(line->wordText(i - 1)) == " "
                              ? renderer.getSpaceWidth(0, line->wordStyle(i - 1))
                              : renderer.getTextAdvanceX(0, line->wordText(i - 1), line->wordStyle(i - 1), tracking);
      std::cout << line->wordXpos(i) - line->wordXpos(i - 1) - advance;
    }
    std::cout << "]}";
  }, true, static_cast<int8_t>(tracking));
  std::cout << "]}\n";
  return text.hadDroppedWords() ? 1 : 0;
}
