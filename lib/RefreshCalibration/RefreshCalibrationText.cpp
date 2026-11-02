#include "RefreshCalibrationText.h"

#include <string.h>

namespace refresh_calibration {

namespace {

// Bytes in the UTF-8 sequence starting with lead; a stray byte counts as one.
size_t sequenceLength(const unsigned char lead) {
  if (lead >= 0xF0 && lead < 0xF8) return 4;
  if (lead >= 0xE0) return lead < 0xF0 ? 3 : 1;
  if (lead >= 0xC0) return 2;
  return 1;
}

}  // namespace

uint32_t nextFixtureLine(const char* corpus, const size_t corpusLength, uint32_t offset, const bool cjk,
                         const int maxWidth, const LineWidth width, const void* context, char* out,
                         const size_t outSize) {
  if (outSize == 0) return offset;
  out[0] = '\0';
  if (corpus == nullptr || corpusLength == 0) return 0;
  while (offset < corpusLength && corpus[offset] == ' ') ++offset;
  if (offset >= corpusLength) offset = 0;

  const char* start = corpus + offset;
  const size_t available = corpusLength - offset;
  size_t fit = 0;
  while (fit < available) {
    size_t next = fit;
    if (cjk) {
      next += sequenceLength(static_cast<unsigned char>(start[next]));
      if (next > available) next = available;
    } else {
      while (next < available && start[next] == ' ') ++next;
      if (next == available) break;  // only spaces left: no word to add
      while (next < available && start[next] != ' ') ++next;
    }
    if (next >= outSize) break;
    memcpy(out, start, next);
    out[next] = '\0';
    if (fit > 0 && width(context, out) > maxWidth) break;
    fit = next;
  }
  if (fit == 0) {
    // A single unit longer than the buffer: take what fits so the page still advances.
    fit = available < outSize - 1 ? available : outSize - 1;
    if (fit == 0) return offset;
  }
  memcpy(out, start, fit);
  out[fit] = '\0';
  offset += static_cast<uint32_t>(fit);
  while (offset < corpusLength && corpus[offset] == ' ') ++offset;
  return offset >= corpusLength ? 0 : offset;
}

}  // namespace refresh_calibration
