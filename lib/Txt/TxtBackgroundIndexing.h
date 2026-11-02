#pragma once

#include <TxtLineBreak.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace TxtBackgroundIndexing {

// Bound each font metric batch so input can interrupt cold-glyph preparation.
inline constexpr size_t PREPARE_BYTES = 128;

inline bool prepareInChunks(const char* text, void* ctx, void (*prepare)(void*, const char*),
                            bool (*shouldYield)(void*)) {
  size_t remaining = std::strlen(text);
  char chunk[PREPARE_BYTES + 1];
  while (remaining > 0) {
    if (shouldYield(ctx)) return false;
    size_t length = std::min(PREPARE_BYTES, remaining);
    if (length < remaining) length = TxtLineBreak::completePrefix(text, length);
    std::memcpy(chunk, text, length);
    chunk[length] = '\0';
    prepare(ctx, chunk);
    text += length;
    remaining -= length;
  }
  return !shouldYield(ctx);
}

// Leave input polling time to commit both debounce edges before starting more layout work.
class QuietGate {
 public:
  static constexpr uint32_t QUIET_MS = 100;

  bool ready(const uint32_t now, const bool inputPending) {
    if (inputPending) {
      lastInput = now;
      return false;
    }
    return static_cast<uint32_t>(now - lastInput) >= QUIET_MS;
  }

 private:
  uint32_t lastInput = 0;
};

}  // namespace TxtBackgroundIndexing
