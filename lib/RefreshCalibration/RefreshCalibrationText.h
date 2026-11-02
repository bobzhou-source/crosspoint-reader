#pragma once

#include <stddef.h>
#include <stdint.h>

// Line breaking for the calibration run's bundled text pages. Pure: the width
// of a candidate line comes from the caller (the renderer on the device).
namespace refresh_calibration {

// Pixel width of a NUL-terminated line.
using LineWidth = int (*)(const void* context, const char* text);

// Writes the next line of a looping corpus into out (NUL-terminated) and
// returns the offset of the line after it. Leading spaces are skipped, then
// whole units are taken while the line fits maxWidth: a Latin word with the
// space before it, or one CJK codepoint. At least one unit is always taken, so
// a word wider than the line cannot stall the page. At the end of the corpus
// the offset wraps to 0, so a rung of any length always has text to show.
uint32_t nextFixtureLine(const char* corpus, size_t corpusLength, uint32_t offset, bool cjk, int maxWidth,
                         LineWidth width, const void* context, char* out, size_t outSize);

}  // namespace refresh_calibration
