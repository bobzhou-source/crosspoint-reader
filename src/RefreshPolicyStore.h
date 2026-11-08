#pragma once

#include <RefreshPolicyConfig.h>

namespace refresh_policy_store {

// Reads /.crosspoint/refresh-policy.json (if present) and makes the result the
// active refresh-policy config. A missing, unreadable or invalid file leaves
// the compiled defaults in force. Main task, SD mounted, no reader open.
const refresh_policy_config::State& load();

// /.crosspoint/liquid-refresh.bin: the LiquidRefresh readout the policy
// learned from manual refreshes (AdaptiveRefreshPolicy::kLearnedStateBytes).
// loadLearned() leaves the policy's defaults when the file is missing or
// rejected; saveLearned() replaces the file atomically. Main task.
bool loadLearned(AdaptiveRefreshPolicy& policy);
bool saveLearned(const uint8_t* bytes, size_t size);

}  // namespace refresh_policy_store
