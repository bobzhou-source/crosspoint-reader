#include "RefreshPolicyStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

namespace refresh_policy_store {

const refresh_policy_config::State& load() {
  using refresh_policy_config::Source;
  using refresh_policy_config::State;
  State state;
  HalFile file;
  if (!Storage.exists(refresh_policy_config::kPath)) {
    state.source = Source::Compiled;
  } else if (!Storage.openFileForRead("RPOL", refresh_policy_config::kPath, file)) {
    state.source = Source::FileUnreadable;
    snprintf(state.detail, sizeof(state.detail), "open failed");
  } else if (file.size() > refresh_policy_config::kMaxFileBytes) {
    state.source = Source::FileInvalid;
    snprintf(state.detail, sizeof(state.detail), "file too large");
  } else {
    // Bounded by kMaxFileBytes (2 KiB); short-lived, so heap rather than the task stack.
    const size_t size = file.size();
    auto buffer = makeUniqueNoThrow<char[]>(size + 1);
    if (!buffer) {
      LOG_ERR("RPOL", "OOM: %u bytes", static_cast<unsigned>(size + 1));
      state.source = Source::FileUnreadable;
      snprintf(state.detail, sizeof(state.detail), "out of memory");
    } else if (file.read(buffer.get(), size) != static_cast<int>(size)) {
      state.source = Source::FileUnreadable;
      snprintf(state.detail, sizeof(state.detail), "read failed");
    } else {
      buffer[size] = '\0';
      state = refresh_policy_config::parse(buffer.get(), size);
    }
  }
  refresh_policy_config::setActive(state);
  const auto& c = state.config;
  LOG_INF("RPOL",
          "Refresh policy %s from %s%s%s: clear=%lu night=%lu cap=%u gray=%lu image=%lu grayImage=%lu frame=%lu "
          "clearAfterImage=%d imageWaveform=%s imageClearRegion=%d",
          AdaptiveRefreshPolicy::kName, refresh_policy_config::sourceName(state.source), state.detail[0] ? " - " : "",
          state.detail, static_cast<unsigned long>(c.clearThresholdPermille),
          static_cast<unsigned long>(c.nightClearThresholdPermille), static_cast<unsigned>(c.safetyCap),
          static_cast<unsigned long>(c.weightGrayscalePermille), static_cast<unsigned long>(c.weightImagePermille),
          static_cast<unsigned long>(c.weightGrayscaleImageExtraPermille),
          static_cast<unsigned long>(c.frameChargePermille), c.clearAfterImage ? 1 : 0,
          refresh_policy_config::imageWaveformName(c.imageWaveform), c.imageClearRegion ? 1 : 0);
  return refresh_policy_config::active();
}

}  // namespace refresh_policy_store
