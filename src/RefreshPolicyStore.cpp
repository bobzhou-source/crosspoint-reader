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

bool loadLearned(AdaptiveRefreshPolicy& policy) {
  constexpr const char* path = AdaptiveRefreshPolicy::kLearnedStatePath;
  uint8_t bytes[AdaptiveRefreshPolicy::kLearnedStateBytes];
  HalFile file;
  if (!Storage.exists(path)) return false;
  if (!Storage.openFileForRead("RPOL", path, file) || file.size() != sizeof(bytes) ||
      file.read(bytes, sizeof(bytes)) != static_cast<int>(sizeof(bytes)) ||
      !policy.loadLearnedState(bytes, sizeof(bytes))) {
    LOG_ERR("RPOL", "Ignoring %s: unreadable or invalid", path);
    return false;
  }
  const auto& w = policy.liquidReadout().w;
  LOG_INF("RPOL", "Liquid refresh readout loaded: debt=%d peak=%d streak=%d bias=%d updates=%lu", w[0], w[1], w[2],
          w[3], static_cast<unsigned long>(policy.liquidUpdates()));
  return true;
}

bool saveLearned(const uint8_t* bytes, const size_t size) {
  constexpr const char* path = AdaptiveRefreshPolicy::kLearnedStatePath;
  char temporary[48];
  snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  bool written = false;
  {
    HalFile file;
    if (Storage.openFileForWrite("RPOL", temporary, file)) {
      written = file.write(bytes, size) == size && file.sync();
      written = file.close() && written;  // Close before renaming.
    }
  }
  if (!written || (Storage.exists(path) && !Storage.remove(path)) || !Storage.rename(temporary, path)) {
    Storage.remove(temporary);
    LOG_ERR("RPOL", "Could not save %s", path);
    return false;
  }
  return true;
}

}  // namespace refresh_policy_store
