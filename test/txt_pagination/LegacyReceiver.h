#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "WidthOracle.h"

// Receiver for the frozen pre-change TxtReaderActivity::loadPageAtOffset
// (legacy/baseline_load_page.inc, sha256 700401614353138c990e5fc693b94d17c48c1e49197bd2ed0a85fa1173c86393,
// verbatim from baseline commit 7f8a9af8). LegacyLoadPage.cpp only renames the receiver; everything the
// loader touches is supplied by the stand-ins below.

struct EpdFontFamily {
  enum Style { REGULAR = 0 };
};

// FNV-1a of the NUL-terminated page text handed to the SD font preparation hook.
inline uint64_t textDigest(const char* text) {
  uint64_t hash = 14695981039346656037ULL;
  for (; *text != '\0'; ++text) hash = (hash ^ static_cast<unsigned char>(*text)) * 1099511628211ULL;
  return hash;
}

class FakeRenderer {
 public:
  explicit FakeRenderer(const WidthOracle& oracle, const bool sdFont = false) : oracle_(oracle), sdFont_(sdFont) {}

  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style) const { return oracle_.width(text); }
  bool isSdCardFont(int) const { return sdFont_; }
  // Records a digest of the page text handed to the SD font preparation hook.
  void ensureSdCardFontReady(int, const char* pageText, uint8_t) const { prepared.push_back(textDigest(pageText)); }

  mutable std::vector<uint64_t> prepared;

 private:
  const WidthOracle& oracle_;
  bool sdFont_;
};

using GfxRenderer = FakeRenderer;

// The frozen Txt::readContent (baseline lib/Txt/Txt.cpp:173-189): a fresh open per call, one seek, one
// bulk read whose signed result is accepted whenever it converts to a positive size_t.
class LegacyTxt {
 public:
  LegacyTxt(std::string path, const size_t fileSize) : path_(std::move(path)), fileSize_(fileSize) {}

  size_t getFileSize() const { return fileSize_; }

  bool readContent(uint8_t* buffer, const size_t offset, const size_t length) const {
    HalFile file;
    if (!Storage.openFileForRead("TXT", path_, file)) return false;
    if (!file.seek(offset)) return false;
    const size_t bytesRead = static_cast<size_t>(file.read(buffer, length));
    return bytesRead > 0;
  }

 private:
  std::string path_;
  size_t fileSize_;
};

class LegacyReceiver {
 public:
  std::unique_ptr<LegacyTxt> txt;
  int cachedFontId = 0;
  int viewportWidth = 0;
  int linesPerPage = 0;

  bool loadPageAtOffset(const GfxRenderer& renderer, size_t offset, std::vector<std::string>& outLines,
                        size_t& nextOffset);
};
