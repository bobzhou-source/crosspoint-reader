#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace CjkDictionary {
constexpr size_t MAX_CHARACTERS = 8;
constexpr size_t RUN_BYTES = MAX_CHARACTERS * 4 + 1;

// Strict UTF-8 decode. Invalid/truncated bytes consume one byte and end a run.
inline size_t characterBytes(const char* text, uint32_t& cp) {
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  cp = *p;
  if (*p < 0x80) return *p ? 1 : 0;
  const size_t n = *p >= 0xC2 && *p <= 0xDF ? 2 : (*p >= 0xE0 && *p <= 0xEF ? 3 : (*p >= 0xF0 && *p <= 0xF4 ? 4 : 0));
  if (!n) {
    cp = 0;
    return 1;
  }
  cp = *p & (0x7F >> n);
  for (size_t i = 1; i < n; ++i) {
    if (!p[i] || (p[i] & 0xC0) != 0x80) {
      cp = 0;
      return 1;
    }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
    cp = 0;
    return 1;
  }
  return n;
}

inline bool isCjk(uint32_t cp) {
  return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x20000 && cp <= 0x323AF) || (cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x30A1 && cp <= 0x30FA) ||
         (cp >= 0xAC00 && cp <= 0xD7A3);
}

// Byte endpoints, not codepoint counts: never truncate a supplementary ideograph.
inline uint8_t prefixes(const char* text, uint8_t (&ends)[MAX_CHARACTERS]) {
  uint8_t count = 0;
  size_t bytes = 0;
  while (count < MAX_CHARACTERS) {
    uint32_t cp;
    const size_t n = characterBytes(text + bytes, cp);
    if (!n || !isCjk(cp)) break;
    bytes += n;
    ends[count++] = static_cast<uint8_t>(bytes);
  }
  return count;
}
}  // namespace CjkDictionary
