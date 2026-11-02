#pragma once

#include <EpdFontData.h>

// The vector tests drive TTF fonts. The built-in compressed-font decompressor is
// only here to observe that a render scope still drops its transient page
// buffers: `pagesResident` is set by the test and cleared by clearCache(), the
// same way a decompressed page buffer is dropped (dispatch coverage for built-in
// fonts lives in FontCacheManagerTest.cpp).
class FontDecompressor {
 public:
  void clearCache() { pagesResident = false; }
  int prewarmCache(const EpdFontData*, const char*) { return 0; }
  void logStats(const char*) {}
  void resetStats() {}

  bool pagesResident = false;
};
