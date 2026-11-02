#pragma once

#include <cstddef>
#include <cstdint>

// Chapter headings in plain-text books.
namespace TxtChapters {

// A heading is a short line of its own; longer lines are prose.
inline constexpr size_t MAX_HEADING_BYTES = 120;
inline constexpr size_t MAX_HEADING_CODEPOINTS = 40;

// Whether one physical line (without its line terminator) is a chapter
// heading: 第…章/节/回/卷/集/部/篇 with Chinese, ASCII or fullwidth numerals,
// the usual named sections (序章, 楔子, 尾声, 番外, 后记, ...), or English
// "Chapter N", "Prologue" and "Epilogue". Leading and trailing whitespace,
// including U+3000, is ignored.
bool isHeading(const char* line, size_t length);

// Trims `line[0, length)` to its visible heading text: whitespace (including
// U+3000 and a BOM) is dropped from both ends. Returns the trimmed start and
// writes the trimmed length.
const char* trimHeading(const char* line, size_t length, size_t& trimmedLength);

}  // namespace TxtChapters
