#pragma once

#include <cstdint>
#include <string>

// Observable SD (.cpfont) mini-cache: `cached` holds what a scope's prewarm put
// there. prewarm(accumulate=false) replaces it, clearCache()/releaseResidentCaches()
// drop it. Used to prove a render scope still resets SD fonts while TTF fonts are
// retained (SD dispatch coverage lives in FontCacheManagerTest.cpp).
class SdCardFont {
 public:
  void clearCache() { cached.clear(); }
  void releaseResidentCaches() { cached.clear(); }
  int prewarm(const char* text, uint8_t, bool, bool, bool accumulate) {
    if (!accumulate) cached.clear();
    cached += text;
    return 0;
  }
  uint8_t resolveStyle(const uint8_t style) const { return style & 0x03; }
  void logStats(const char*) {}
  void resetStats() {}

  std::string cached;
};
