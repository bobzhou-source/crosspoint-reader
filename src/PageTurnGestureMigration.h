#pragma once

#include <cstdint>

// Page-turn gesture settings shipped with SWIPE_ONLY as the untouched default,
// and wrote it explicitly into settings.json. The default is now TAP_AND_SWIPE,
// so files from before this schema need a one-time upgrade. A schema key marks
// a file as post-migration; after that an explicit SWIPE_ONLY choice survives.
namespace page_turn_gesture {

constexpr uint8_t SCHEMA_VERSION = 1;

// Upgrades the pair only when it still holds the old default in both
// directions and the file carries no schema marker. `legacyDefault` and
// `replacement` are the CrossPointSettings enum values. Returns true when the
// values changed, which asks the caller to resave the file with the schema key.
inline bool upgradeLegacyDefault(uint8_t& next, uint8_t& previous, const bool schemaPresent,
                                 const uint8_t legacyDefault, const uint8_t replacement) {
  if (schemaPresent || next != legacyDefault || previous != legacyDefault) return false;
  next = replacement;
  previous = replacement;
  return true;
}

}  // namespace page_turn_gesture
