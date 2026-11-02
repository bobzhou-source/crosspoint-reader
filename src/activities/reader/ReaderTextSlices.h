#pragma once

#include <FreeInkUICore.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

// Non-owning, width-derived windows over saved text. Only the visible row is
// copied; the source snapshot remains intact and every byte stays reachable.
namespace ReaderTextSlices {
constexpr uint8_t LINES = 3;
constexpr size_t MAX_LINE_BYTES = 220;  // FreeInkUI's per-line layout bound.

struct Cursor {
  const char* text = nullptr;
  size_t row = 0;
  size_t bytes = 0;
};

inline bool space(const char c) { return c == ' ' || c == '\n'; }

inline size_t lineEnd(const freeink::ui::DrawTarget& target, const char* text,
                      const freeink::ui::TextStyle style, const int16_t width) {
  char measured[MAX_LINE_BYTES + 1];
  size_t fit = 0;
  size_t scan = 0;
  while (text[scan] && text[scan] != '\n') {
    size_t end = scan;
    while (text[end] && !space(text[end])) ++end;
    if (end > MAX_LINE_BYTES) break;
    memcpy(measured, text, end);
    measured[end] = '\0';
    if (target.measureText(style.font, measured, style).width > width) break;
    fit = end;
    if (!text[end] || text[end] == '\n') break;
    scan = end;
    while (text[scan] == ' ') ++scan;
  }
  if (fit != 0) return fit;
  // A URL or unbroken script can be wider than a line. Match the renderer's
  // UTF-8 wrap, rather than dropping it or emitting an ellipsis.
  while (text[fit] && !space(text[fit])) {
    size_t next = fit + 1;
    while ((static_cast<unsigned char>(text[next]) & 0xc0) == 0x80) ++next;
    if (next > MAX_LINE_BYTES) break;
    memcpy(measured, text, next);
    measured[next] = '\0';
    if (fit != 0 && target.measureText(style.font, measured, style).width > width) break;
    fit = next;
  }
  return fit;
}

inline size_t length(const freeink::ui::DrawTarget& target, const char* text,
                     const freeink::ui::TextStyle style, const int16_t width) {
  size_t end = 0;
  for (uint8_t line = 0; line < LINES && text[end]; ++line) {
    while (text[end] == ' ') ++end;
    if (!text[end]) break;
    if (text[end] == '\n') {
      ++end;
      continue;
    }
    end += lineEnd(target, text + end, style, width);
    if (text[end] == '\n') ++end;
  }
  // Never carry half of a normal word into the next row. Only a single
  // unbroken token longer than a complete window needs a visual continuation.
  if (end != 0 && text[end] && !space(text[end]) && !space(text[end - 1])) {
    size_t boundary = end;
    while (boundary != 0 && !space(text[boundary - 1])) --boundary;
    if (boundary != 0) end = boundary;
  }
  return end;
}

inline size_t count(const freeink::ui::DrawTarget& target, const char* text,
                    const freeink::ui::TextStyle style, const int16_t width) {
  size_t rows = 0;
  while (text && *text) {
    while (space(*text)) ++text;
    if (!*text) break;
    const size_t n = length(target, text, style, width);
    if (n == 0) break;
    text += n;
    ++rows;
  }
  return rows;
}

inline void copyRow(const freeink::ui::DrawTarget& target, const char* source,
                    const freeink::ui::TextStyle style, const int16_t width,
                    const size_t index, char* output, const size_t capacity, Cursor& cursor) {
  output[0] = '\0';
  if (!cursor.text || index < cursor.row) cursor = {source, 0, 0};
  while (cursor.text && *cursor.text) {
    while (space(*cursor.text)) ++cursor.text;
    if (!*cursor.text) return;
    if (cursor.bytes == 0) cursor.bytes = length(target, cursor.text, style, width);
    if (cursor.bytes == 0) return;
    if (cursor.row == index) {
      size_t copied = std::min(cursor.bytes, capacity - 1);
      while (copied != 0 && space(cursor.text[copied - 1])) --copied;
      memcpy(output, cursor.text, copied);
      output[copied] = '\0';
      return;
    }
    cursor.text += cursor.bytes;
    cursor.bytes = 0;
    ++cursor.row;
  }
}
}  // namespace ReaderTextSlices
