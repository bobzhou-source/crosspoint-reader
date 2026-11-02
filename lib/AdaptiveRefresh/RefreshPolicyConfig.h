#pragma once

#include <stddef.h>
#include <stdint.h>

#include "AdaptiveRefreshPolicy.h"

// Runtime override of AdaptiveRefreshPolicy's tunables from
// /.crosspoint/refresh-policy.json, so a calibration result applies without a
// firmware install. Deleting the file restores the compiled defaults at the
// next boot (or when Settings -> System -> Refresh calibration reloads it).
//
// File format (UTF-8 JSON object, at most kMaxFileBytes):
//   {"schema": 1, "policy": "adaptive-content-v2",
//    "clear_threshold_permille": 2400, "night_clear_threshold_permille": 3000,
//    "safety_cap": 60, "weight_grayscale_permille": 250,
//    "weight_image_permille": 1000, "weight_grayscale_image_extra_permille": 500,
//    "frame_charge_permille": 0, "clear_after_image": true,
//    "image_waveform": "quality", "meta": {...}}
// "schema" (must be 1) is required; "policy", when present, must equal
// AdaptiveRefreshPolicy::kName. Every tunable is optional and falls back to
// the compiled default, except that a missing night threshold follows the
// day threshold. Numeric tunables must be JSON integers; values outside their
// range are clamped (and reported). "clear_after_image" must be a JSON bool
// and "image_waveform" the string "quality" or "fast". "meta" (any object) is
// ignored. Any other key, a wrongly typed tunable, an unknown waveform name or
// malformed JSON rejects the whole file: the compiled policy runs and the
// reason is reported.
namespace refresh_policy_config {

using Config = AdaptiveRefreshPolicy::Config;

static constexpr char kPath[] = "/.crosspoint/refresh-policy.json";
static constexpr size_t kMaxFileBytes = 2048;
static constexpr int kSchema = 1;

struct Range {
  uint32_t min;
  uint32_t max;
};
static constexpr Range kThresholdRange{100, 20000};
static constexpr Range kSafetyCapRange{1, 500};
static constexpr Range kWeightRange{0, 16000};
static constexpr Range kFrameChargeRange{0, 1000};

enum class Source : uint8_t {
  Compiled,      // no file
  File,          // file applied as written
  FileClamped,   // file applied after clamping out-of-range values
  FileInvalid,   // file rejected; compiled defaults run
  FileUnreadable // file present but could not be read; compiled defaults run
};

struct State {
  Config config{};
  Source source = Source::Compiled;
  uint8_t clampedFields = 0;
  // Short reason for FileInvalid/FileUnreadable or the first clamped key.
  char detail[48] = {};
};

// Parses a refresh-policy.json body. Never fails: an invalid file yields the
// compiled Config with Source::FileInvalid and a reason in detail.
State parse(const char* data, size_t length);

// Stable identifier for logs/metadata ("compiled", "file", ...).
const char* sourceName(Source source);
// "quality" or "fast", the image_waveform spelling of the file format.
inline const char* imageWaveformName(const AdaptiveRefreshPolicy::ImageWaveform waveform) {
  return waveform == AdaptiveRefreshPolicy::ImageWaveform::Fast ? "fast" : "quality";
}
// True when the config in force came from the SD file.
inline bool fromFile(Source source) { return source == Source::File || source == Source::FileClamped; }

// Process-wide config used when a reader creates its policy. Main task only.
const State& active();
void setActive(const State& state);

}  // namespace refresh_policy_config
