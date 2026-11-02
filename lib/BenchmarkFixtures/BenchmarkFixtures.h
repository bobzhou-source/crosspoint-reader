#pragma once

namespace benchmark_fixtures {

enum class PrepareResult {
  Ready,
  StorageUnavailable,
  Conflict,
  WriteFailed,
};

PrepareResult prepare();
const char* epubPath();
const char* txtPath();
const char* bitmapPath();

}  // namespace benchmark_fixtures
