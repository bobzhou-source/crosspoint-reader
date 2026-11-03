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
  Requested = 10     // another reader request (reopen without fast resume, picture fallback, ...)
};

// Slot 5 bit flags.
inline constexpr uint32_t CLEAR_FLAG_NIGHT = 1u << 0;
inline constexpr uint32_t CLEAR_FLAG_POLICY_FILE = 1u << 1;
inline constexpr uint32_t CLEAR_FLAG_IMAGE = 1u << 2;
inline constexpr uint32_t CLEAR_FLAG_GRAYSCALE = 1u << 3;

ClearCause clearCause(const AdaptiveRefreshPolicy::ClearRecord& record);

// One instant row per committed clear: manual_refresh when the user asked for it,
// refresh_clear otherwise. Slots: cause, fast turns since the previous clear, debt
// permille, picture pages and grayscale pages among those turns, flags, book ID,
// threshold permille in force.
Event refreshClearEvent(const AdaptiveRefreshPolicy::ClearRecord& record, bool policyFile, uint32_t bookId,
                        uint64_t nowUs);

}  // namespace benchmark
