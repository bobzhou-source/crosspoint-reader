#pragma once

#include <AdaptiveRefreshPolicy.h>

#include "BenchmarkBuffer.h"

namespace benchmark {

// Slot 0 of refresh_clear / manual_refresh rows. Values are persisted: append only.
enum class ClearCause : uint8_t {
  Unlabelled = 0,    // the caller cleared although the policy chose a fast refresh
  Initial = 1,       // first frame of a reader session
  Debt = 2,          // ghost debt reached the threshold
  SafetyCap = 3,     // too many fast refreshes in a row
  Image = 4,         // the page after an inline picture
  Popup = 5,         // an indexing popup was drawn over the page
  Manual = 6,        // the user's refresh command (button, reader menu, control center)
  Overlay = 7,       // a closing overlay promoted the repaint (option popup, night toggle)
  Polarity = 8,      // output inversion flipped
  InvalidFrame = 9,  // framebuffer metrics unreadable
  Requested = 10,    // another reader request (reopen without fast resume, picture fallback, ...)
  ImageSticking = 11  // LiquidRefresh CfC v2: the page overwrote a polarized picture
};

// Slot 5 bit flags.
inline constexpr uint32_t CLEAR_FLAG_NIGHT = 1u << 0;
inline constexpr uint32_t CLEAR_FLAG_POLICY_FILE = 1u << 1;
inline constexpr uint32_t CLEAR_FLAG_IMAGE = 1u << 2;
inline constexpr uint32_t CLEAR_FLAG_GRAYSCALE = 1u << 3;
// The clear covered only the row's "region" rect (image_clear_region); the
// turn counts carry on to the next full clear.
inline constexpr uint32_t CLEAR_FLAG_REGION = 1u << 4;
// Bits 5-7: AdaptiveRefreshPolicy::RegionFallback, why a picture clear in
// region mode still covered the whole screen (0 none).
inline constexpr unsigned CLEAR_REGION_FALLBACK_SHIFT = 5;
inline constexpr uint32_t CLEAR_REGION_FALLBACK_MASK = 7u << CLEAR_REGION_FALLBACK_SHIFT;

ClearCause clearCause(const AdaptiveRefreshPolicy::ClearRecord& record);

// One instant row per committed clear: manual_refresh when the user asked for it,
// refresh_clear otherwise. Slots: cause, fast turns since the previous clear, debt
// permille, picture pages and grayscale pages among those turns, flags, book ID,
// threshold permille in force. "region": the logical rect of a region clear
// (x, y, w, h), all zero for a whole-screen clear.
Event refreshClearEvent(const AdaptiveRefreshPolicy::ClearRecord& record, bool policyFile, uint32_t bookId,
                        uint64_t nowUs);

}  // namespace benchmark
