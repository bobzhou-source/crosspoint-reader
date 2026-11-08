#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

struct WifiResult {
  bool connected = false;
  std::string ssid;
  std::string ip;
};

struct KeyboardResult {
  std::string text;
};

struct MenuResult {
  int action = -1;
  uint8_t orientation = 0;
  uint8_t pageTurnOption = 0;
};

struct ChapterResult {
  int spineIndex = 0;
  std::string anchor;
};

struct PercentResult {
  int percent = 0;
};

struct IntervalResult {
  uint32_t value = 0;
};

struct PageResult {
  uint32_t page = 0;
};

struct ProgressChangeResult {
  int spineIndex = 0;
  int page = 0;
  int totalPages = 0;
  std::string xpath;
  float percentage = 0.0f;
  bool hasSavedProgress = false;
  // Exact visible-codepoint offset within spineIndex, when the source (a bookmark) has one.
  // Preferred over xpath/percentage on resolution: it is immune to re-pagination.
  bool hasVisibleTextOffset = false;
  uint32_t visibleTextOffset = 0;
};

enum class NetworkMode;

struct NetworkModeResult {
  NetworkMode mode;
};

struct FootnoteResult {
  std::string href;
};

struct FilePathResult {
  std::string path;
};

enum class BenchmarkOutcome : uint8_t { Complete, Cancelled, LoadFailed, RenderFailed, NoProgress, Timeout, RecorderFailed };

struct BenchmarkResult {
  BenchmarkOutcome outcome = BenchmarkOutcome::Cancelled;
};

// How long the picture viewer showed a picture, and over how much of the
// screen: the reader books it as image exposure for its refresh policy.
struct PictureViewerResult {
  uint32_t dwellMs = 0;
  uint16_t areaPermille = 0;    // 0 = no picture was shown
  uint16_t ditherPermille = 0;  // mid-gray (dithered) share of the picture
  uint64_t tileMask = 0;        // AdaptiveRefreshPolicy tiles; 0 = all
};

using ResultVariant =
    std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult, ChapterResult, PercentResult, IntervalResult,
                 PageResult, ProgressChangeResult, NetworkModeResult, FootnoteResult, FilePathResult, BenchmarkResult,
                 PictureViewerResult>;

struct ActivityResult {
  bool isCancelled = false;
  ResultVariant data;

  explicit ActivityResult() = default;

  template <typename ResultType>
    requires std::is_constructible_v<ResultVariant, ResultType&&>
  // cppcheck-suppress noExplicitConstructor
  ActivityResult(ResultType&& result) : data{std::forward<ResultType>(result)} {}
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;
