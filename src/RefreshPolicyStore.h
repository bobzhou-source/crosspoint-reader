#pragma once

#include <RefreshPolicyConfig.h>

namespace refresh_policy_store {

// Reads /.crosspoint/refresh-policy.json (if present) and makes the result the
// active refresh-policy config. A missing, unreadable or invalid file leaves
// the compiled defaults in force. Main task, SD mounted, no reader open.
const refresh_policy_config::State& load();

}  // namespace refresh_policy_store
