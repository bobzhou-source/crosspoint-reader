#pragma once

#include <Utf8.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Deterministic stand-in for GfxRenderer::getTextAdvanceX, shared by the frozen legacy loader and
// the production walker. The simple model is additive (10px per codepoint, 20px for wide ones,
// 0 for combining marks) so goldens can be derived by hand. The complex model has the properties
// that make real measurement non-additive: 12.4 fixed-point advances rounded once per string,
// kerning, an "fi" ligature, whole-string rescaling when a wide codepoint is present, and a
// length quirk that makes width non-monotonic in prefix length.
class WidthOracle {
 public:
  enum class Model { Simple, Complex };

  explicit WidthOracle(const Model model = Model::Simple) : model_(model) {}

  // Measures the NUL-terminated string like the renderer does.
  int width(const char* text) const {
    const size_t length = std::strlen(text);
    calls++;
    inputBytes += length;
    maxInputBytes = std::max(maxInputBytes, length);
    if (trace != nullptr) trace->emplace_back(text, length);
    return model_ == Model::Simple ? simpleWidth(text) : complexWidth(text, length);
  }

  void resetCounters() {
    calls = 0;
    inputBytes = 0;
    maxInputBytes = 0;
  }

  mutable uint64_t calls = 0;
  mutable uint64_t inputBytes = 0;
  mutable size_t maxInputBytes = 0;
  // Receives a copy of every queried string when set. Allocates: leave null while counting allocations.
  mutable std::vector<std::string>* trace = nullptr;

 private:
  static bool isWide(const uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0xD7FF) || (cp >= 0xF900 && cp <= 0xFAFF) || cp >= 0x10000;
  }
  static bool isCombining(const uint32_t cp) { return cp >= 0x300 && cp <= 0x36F; }

  static int simpleWidth(const char* text) {
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    int px = 0;
    while (uint32_t cp = utf8NextCodepoint(&p)) {
      if (!isCombining(cp)) px += isWide(cp) ? 20 : 10;
    }
    return px;
  }

  static int32_t advance16(const uint32_t cp, const bool stringHasWide) {
    if (isWide(cp)) return 15 * 16 + 4;  // wide glyphs are not rescaled
    int32_t adv = 12 * 16;
    if (cp == ' ') {
      adv = 4 * 16 + 8;
    } else if (cp >= '0' && cp <= '9') {
      adv = 8 * 16;
    } else if (cp >= 'a' && cp <= 'z') {
      adv = 7 * 16 + static_cast<int32_t>((cp * 7) % 5) * 6;
    } else if (cp >= 'A' && cp <= 'Z') {
      adv = 9 * 16 + static_cast<int32_t>((cp * 3) % 4) * 5;
    } else if (cp < 0x80) {
      adv = 5 * 16 + 3;
    } else if (cp < 0x800) {
      adv = 8 * 16 + 9;
    }
    return stringHasWide ? adv * 5 / 4 : adv;
  }

  static int32_t kern16(const uint32_t prev, const uint32_t cp) {
    if (prev == 'A' && cp == 'V') return -24;
    if (prev == 'T' && cp == 'o') return -20;
    if (prev == 'f' && cp == 'i') return -16;
    if (prev == ' ' && cp == 'T') return -8;
    if (prev == 'l' && cp == 'l') return -4;
    return 0;
  }

  static int complexWidth(const char* text, const size_t length) {
    const auto* scan = reinterpret_cast<const unsigned char*>(text);
    bool hasWide = false;
    while (uint32_t cp = utf8NextCodepoint(&scan)) {
      if (isWide(cp)) {
        hasWide = true;
        break;
      }
    }

    const auto* p = reinterpret_cast<const unsigned char*>(text);
    int32_t sum = 0;
    uint32_t prev = 0;
    while (uint32_t cp = utf8NextCodepoint(&p)) {
      if (isCombining(cp)) continue;
      if (prev != 0) sum += kern16(prev, cp);
      sum += advance16(cp, hasWide);
      prev = cp;
    }

    int px = sum <= 0 ? 0 : (sum + 8) / 16;
    if (length != 0 && length % 64 == 0) px = std::max(0, px - 12);  // non-monotonic quirk
    return px;
  }

  Model model_;
};
