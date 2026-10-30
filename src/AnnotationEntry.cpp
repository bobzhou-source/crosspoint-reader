#include "AnnotationEntry.h"

#include <algorithm>

std::string annotationText(const std::string_view text, const size_t maxBytes) {
  std::string result;
  result.reserve(std::min(text.size(), maxBytes));
  bool space = false;
  for (size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c <= 0x20 || c == 0x7f) {
      space = !result.empty();
      ++i;
      continue;
    }
    // Keep complete UTF-8 sequences when the byte budget runs out. Reject invalid
    // sequences rather than emitting broken text into the keyboard or Markdown.
    size_t width = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3
                                                               : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (width == 0 || i + width > text.size()) {
      ++i;
      continue;
    }
    bool valid = true;
    for (size_t j = 1; j < width; ++j) {
      if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80) valid = false;
    }
    if (width >= 3) {
      const auto second = static_cast<unsigned char>(text[i + 1]);
      if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0) ||
          (c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90)) valid = false;
    }
    if (!valid) {
      ++i;
      continue;
    }
    if (result.size() + width + (space ? 1 : 0) > maxBytes) break;
    if (space) result.push_back(' ');
    result.append(text.data() + i, width);
    space = false;
    i += width;
  }
  return result;
}
