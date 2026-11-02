#pragma once

#include <AdaptiveRefreshPolicy.h>
#include <stddef.h>
#include <stdint.h>

// Sequencing and logging for the on-device refresh calibration run
// (Settings -> System -> Refresh calibration). Pure: no display, no storage.
//
// The run measures, per content class and polarity, how many fast refreshes
// (and how much ghost debt) Robert accepts before ghosting bothers him. Every
// rung starts from a clearing refresh, shows N fast content frames, then a
// probe screen he photographs and rates 0 (invisible) / 1 (faint, fine) /
// 2 (bothers me). Each polarity block opens with a clean reference rung (clear,
// then the probe without fast frames) that anchors the photo analysis.
//
// Per series an adaptive staircase over kFastCounts finds the rating-1/2
// boundary: the first series starts at 12 fast frames, later ones at the
// boundary of the most similar finished series; the search widens by 1, 2, 4
// levels until a "2" and a "<=1" bracket it, then bisects. A series typically
// needs 2-3 rungs, so the whole run is about 25 rungs.
namespace refresh_calibration {

enum class Polarity : uint8_t { Light = 0, Night = 1 };
enum class ContentClass : uint8_t { DenseLatin = 0, DenseCjk, Sparse, GrayImage, Menu };
enum class RungKind : uint8_t { Reference = 0, Probe };

static constexpr uint8_t kClassCount = 5;
static constexpr uint8_t kFastCounts[] = {2, 4, 8, 12, 16, 24, 32, 48};
static constexpr int8_t kLevels = sizeof(kFastCounts);
static constexpr int8_t kFirstStartLevel = 3;  // 12 fast frames
// Ratings beyond this bracket a series no matter what (noisy, non-monotone ratings).
static constexpr uint8_t kMaxRungsPerSeries = 6;
static constexpr uint8_t kRatingInvisible = 0;
static constexpr uint8_t kRatingFine = 1;
static constexpr uint8_t kRatingBothers = 2;
static constexpr int kLogSchema = 1;

const char* polarityName(Polarity polarity);
const char* className(ContentClass cls);
// Short code used in the on-screen rung ID ("LAT", "CJK", ...).
const char* classCode(ContentClass cls);
bool parsePolarity(const char* name, Polarity& out);
bool parseClass(const char* name, ContentClass& out);
constexpr uint8_t classBit(ContentClass cls) { return static_cast<uint8_t>(1u << static_cast<uint8_t>(cls)); }

struct Rung {
  RungKind kind = RungKind::Reference;
  Polarity polarity = Polarity::Light;
  ContentClass cls = ContentClass::DenseLatin;  // meaningless for references
  int8_t level = -1;                            // index into kFastCounts; -1 for references
  uint8_t seq = 0;                              // 0-based position in the run (= ratings logged before it)

  uint8_t fastCount() const { return level < 0 ? 0 : kFastCounts[level]; }
  // Human-readable ID shown large on the probe: "07 L-LAT-12", "00 L-REF".
  void formatId(char* out, size_t capacity) const;
  bool operator==(const Rung&) const = default;
};

struct Series {
  int8_t lo = -1;      // highest level rated <= 1
  int8_t hi = kLevels; // lowest level rated 2
  int8_t start = kFirstStartLevel;
  uint8_t rungs = 0;
  uint8_t widen = 0;   // widening steps taken while only one side is known

  bool done() const { return hi - lo <= 1 || rungs >= kMaxRungsPerSeries; }
  // Highest level the series accepted, clamped to the ladder (0 when none).
  int8_t boundary() const { return lo < 0 ? 0 : lo; }
};

class Ladder {
 public:
  // skipMask: classBit() of classes this run cannot show (e.g. no CJK glyphs).
  explicit Ladder(uint8_t skipMask = 0);

  uint8_t skipMask() const { return skipMask_; }
  bool done() const { return phase_ >= kPhaseCount; }
  // The rung to show next. Only meaningful while !done().
  Rung next() const;
  // Books a rating for exactly next(); false (no state change) for any other
  // rung or a rating outside 0..2.
  bool rate(const Rung& rung, uint8_t rating);

  uint8_t rated() const { return rated_; }
  // Rated rungs plus a 2-per-unfinished-series estimate of what is left.
  uint8_t estimatedTotal() const;
  const Series& series(Polarity polarity, ContentClass cls) const;

 private:
  // Phase order: light reference, light classes, night reference, night classes.
  static constexpr uint8_t kPhaseCount = 2 * (1 + kClassCount);
  static bool phaseIsReference(uint8_t phase) { return phase % (1 + kClassCount) == 0; }
  static Polarity phasePolarity(uint8_t phase) { return phase < 1 + kClassCount ? Polarity::Light : Polarity::Night; }
  static ContentClass phaseClass(uint8_t phase) {
    return static_cast<ContentClass>(phase % (1 + kClassCount) - 1);
  }
  Series& seriesAt(Polarity polarity, ContentClass cls);
  void skipFinishedPhases();
  int8_t startLevelFor(Polarity polarity, ContentClass cls) const;

  Series series_[2][kClassCount];
  uint8_t skipMask_;
  uint8_t phase_ = 0;
  uint8_t rated_ = 0;
};

// What the run measured while showing one rung's fast frames.
struct RungMeasurement {
  uint32_t pixelCount = 0;
  uint16_t frames = 0;          // fast content frames actually shown
  uint64_t changedPixels = 0;   // policy's per-frame change estimate, unweighted, summed
  uint32_t weightedDebt = 0;    // debt the active policy config accumulated
  uint16_t policyClearAt = 0;   // 1-based fast frame the active config would have cleared at; 0 = none
  uint32_t meanInkPixels = 0;
  bool grayscale = false;       // visible grayscale pass on the content frames
  bool image = false;
};

struct SessionInfo {
  const char* session = "";
  const char* firmware = "";
  const char* fontName = "";
  uint32_t fontId = 0;
  uint32_t pointSize = 0;
  bool antialias = false;
  uint8_t skipMask = 0;
  uint32_t pixelCount = 0;
  const char* policySource = "";
  AdaptiveRefreshPolicy::Config config{};
};

// One JSON object per line, newline-terminated. Return the line length, or 0
// when it does not fit (the caller must not write a truncated row).
size_t formatSessionRow(char* out, size_t capacity, const SessionInfo& info);
size_t formatRungRow(char* out, size_t capacity, const Rung& rung, const RungMeasurement& m, uint8_t rating,
                     uint32_t elapsedMs);
size_t formatCompleteRow(char* out, size_t capacity, const Ladder& ladder);

// Rebuilds a run from its log, line by line. The first line must be the
// session row; every rung row must be exactly the rung the ladder expected.
class Replay {
 public:
  enum class Status : uint8_t { Ok, NeedSession, BadSession, BadRow, Mismatch };
  Status feed(const char* line, size_t length);
  bool started() const { return started_; }
  const Ladder& ladder() const { return ladder_; }

 private:
  Ladder ladder_;
  bool started_ = false;
};

}  // namespace refresh_calibration
