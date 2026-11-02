#pragma once

#include <cstdint>

namespace benchmark {

// Awake battery sampling cadence for the session log (battery_sample events).
constexpr uint32_t BATTERY_SAMPLE_INTERVAL_MS = 5 * 60 * 1000;

// battery_sample slot 2: why the sample was taken. Frozen log values.
enum class BatteryReason : uint8_t { Periodic = 0, SessionStart = 1, SleepEntry = 2 };

// Wrap-safe: a sample is due when none was taken for this session yet, or a
// full interval has elapsed since the last one.
inline bool batterySampleDue(const bool haveSample, const uint32_t nowMs, const uint32_t lastSampleMs) {
  return !haveSample || static_cast<uint32_t>(nowMs - lastSampleMs) >= BATTERY_SAMPLE_INTERVAL_MS;
}

}  // namespace benchmark
