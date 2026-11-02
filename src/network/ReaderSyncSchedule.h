#pragma once

// Pure scheduling policy for the private Wi-Fi Reader sync. No Arduino, Wi-Fi,
// storage or JSON dependencies: the firmware gathers a ScheduleState and this
// header answers "should a sync start now, and why?". It is exercised by the
// host tests in test/reader_sync_schedule.

#include <cstdint>
#include <cstring>

namespace reader_sync {

// reader-sync.json "schedule". A missing key means Events. "interval_seconds"
// keeps its meaning in both modes: the Interval-mode poll period, and the
// charging/continuation period in Events mode.
enum class Schedule : uint8_t { Events = 0, Interval = 1 };

inline const char* scheduleName(const Schedule schedule) {
  return schedule == Schedule::Interval ? "interval" : "events";
}

// Strict: only the two frozen spellings are accepted.
inline bool parseSchedule(const char* text, Schedule& out) {
  if (text == nullptr) return false;
  if (std::strcmp(text, "events") == 0) { out = Schedule::Events; return true; }
  if (std::strcmp(text, "interval") == 0) { out = Schedule::Interval; return true; }
  return false;
}

// Why a run started. Numeric values are written to the session log
// (sync_run slot 0) and are therefore frozen.
enum class Trigger : uint8_t {
  None = 0,
  Manual = 1,    // "Sync now" or enabling the service
  Wake = 2,      // first Home after boot/wake (Interval mode: first run after boot)
  PreSleep = 3,  // pending annotation uploads at deep-sleep entry
  Charging = 4,  // USB power present, interval elapsed (Events mode)
  Interval = 5,  // fixed-period poll (Interval mode)
  Continue = 6,  // previous run reported more bounded work
};

inline const char* triggerName(const Trigger trigger) {
  switch (trigger) {
    case Trigger::Manual: return "manual";
    case Trigger::Wake: return "wake";
    case Trigger::PreSleep: return "pre_sleep";
    case Trigger::Charging: return "charging";
    case Trigger::Interval: return "interval";
    case Trigger::Continue: return "continue";
    case Trigger::None: break;
  }
  return "none";
}

constexpr int64_t MIN_VALID_UTC = 1704067200;  // same floor the protocol clock check uses
constexpr uint32_t AGE_UNKNOWN = UINT32_MAX;

// Wake sync is skipped when the last successful sync is younger than this.
constexpr uint32_t WAKE_DEBOUNCE_SECONDS = 15 * 60;
// Wake and pre-sleep syncs are skipped when any attempt (even a failed one) is
// younger than this, so an offline device does not scan for Wi-Fi on every wake.
constexpr uint32_t RETRY_DEBOUNCE_SECONDS = 5 * 60;
// Hard upper bound on how long deep sleep may wait for the pre-sleep sync
// before the worker is told to stop. Unwinding an in-flight request adds at
// most the HTTP timeout on top.
constexpr uint32_t PRE_SLEEP_BUDGET_MS = 20000;

// Seconds from `thenUtc` to `nowUtc`, or AGE_UNKNOWN when either clock value is
// unusable (unset RTC, never synced, clock moved backwards).
inline uint32_t ageSeconds(const int64_t nowUtc, const int64_t thenUtc) {
  if (nowUtc < MIN_VALID_UTC || thenUtc < MIN_VALID_UTC || thenUtc > nowUtc) return AGE_UNKNOWN;
  const int64_t age = nowUtc - thenUtc;
  return age >= static_cast<int64_t>(AGE_UNKNOWN) ? AGE_UNKNOWN - 1 : static_cast<uint32_t>(age);
}

inline bool within(const uint32_t age, const uint32_t windowSeconds) {
  return age != AGE_UNKNOWN && age < windowSeconds;
}

// Wrap-safe millis() comparison.
inline bool deadlineReached(const uint32_t nowMs, const uint32_t deadlineMs) {
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

struct ScheduleState {
  Schedule schedule = Schedule::Events;
  bool configured = false;       // a valid reader-sync.json is present
  bool enabled = false;
  bool manualRequested = false;  // "Sync now" pending
  bool workerActive = false;
  bool radioForeground = false;  // another feature owns Wi-Fi right now
  bool uiPermitted = false;      // healthy render, not a benchmark workload
  bool homeShown = false;        // the Home activity is on screen
  bool bookOpening = false;      // reader is loading/indexing its first page
  bool externalPower = false;    // USB/charger present
  bool sleeping = false;         // evaluating the deep-sleep entry window
  bool wakePending = false;      // boot/wake sync not yet consumed
  bool continuation = false;     // last run ended with more bounded work
  bool pendingUploads = false;   // annotations saved since the last clean upload pass
  bool parkedUntilWake = false;  // server asked us to stay quiet until next wake
  bool rateLimited = false;      // last run ended 429
  uint32_t nowMs = 0;
  uint32_t nextAttemptMs = 0;    // back-off after cancel/failure/continuation, and the in-boot periodic deadline
  uint32_t intervalSeconds = 900;  // reader-sync.json "interval_seconds"
  uint32_t lastSuccessAgeS = AGE_UNKNOWN;
  uint32_t lastAttemptAgeS = AGE_UNKNOWN;
};

struct Decision {
  Trigger trigger = Trigger::None;
  // The wake event was evaluated and deliberately skipped (debounce): the caller
  // must clear its wake-pending flag so it does not fire later in the session.
  bool consumeWake = false;
};

inline Decision decide(const ScheduleState& s) {
  Decision d;
  if (!s.configured || s.workerActive || s.radioForeground || s.parkedUntilWake) return d;
  const bool due = deadlineReached(s.nowMs, s.nextAttemptMs);
  // Server back-off beats every trigger, including a manual one.
  if (s.rateLimited && !due) return d;

  if (s.sleeping) {
    if (s.enabled && s.schedule == Schedule::Events && s.pendingUploads &&
        !within(s.lastAttemptAgeS, RETRY_DEBOUNCE_SECONDS)) {
      d.trigger = Trigger::PreSleep;
    }
    return d;
  }

  if (!s.uiPermitted) return d;
  if (s.manualRequested) { d.trigger = Trigger::Manual; return d; }
  if (!s.enabled) return d;

  if (s.schedule == Schedule::Interval) {
    if (due) d.trigger = s.wakePending ? Trigger::Wake : Trigger::Interval;
    return d;
  }

  // Events mode never touches the radio while a book is opening.
  if (s.bookOpening) return d;
  if (s.continuation && due) { d.trigger = Trigger::Continue; return d; }
  if (s.wakePending && s.homeShown) {
    if (within(s.lastSuccessAgeS, WAKE_DEBOUNCE_SECONDS) || within(s.lastAttemptAgeS, RETRY_DEBOUNCE_SECONDS)) {
      d.consumeWake = true;
    } else if (due) {
      d.trigger = Trigger::Wake;
      return d;
    }
  }
  // Charging syncs are paced by the persisted run age, so a reboot or a wake-time sync
  // does not restart the interval from zero.
  const uint32_t lastRunAgeS = s.lastSuccessAgeS < s.lastAttemptAgeS ? s.lastSuccessAgeS : s.lastAttemptAgeS;
  if (s.externalPower && due && !within(lastRunAgeS, s.intervalSeconds)) d.trigger = Trigger::Charging;
  return d;
}

enum class WaitReason : uint8_t {
  Ready,
  Unconfigured,
  WorkerActive,
  WifiInUse,
  UntilWake,
  Backoff,
  Foreground,
  Disabled,
  BookOpening,
  Home,
  NextEvent,
  NoSavedNetwork
};

inline WaitReason explainDecision(const ScheduleState& s, const Decision& decision, bool hasSavedNetwork = true) {
  if (!s.configured) return WaitReason::Unconfigured;
  if (s.workerActive) return WaitReason::WorkerActive;
  if (s.radioForeground) return WaitReason::WifiInUse;
  if (s.parkedUntilWake) return WaitReason::UntilWake;
  if (s.rateLimited && !deadlineReached(s.nowMs, s.nextAttemptMs)) return WaitReason::Backoff;
  if (!hasSavedNetwork) return WaitReason::NoSavedNetwork;
  if (decision.trigger != Trigger::None) return WaitReason::Ready;
  if (!s.uiPermitted) return WaitReason::Foreground;
  if (!s.enabled) return WaitReason::Disabled;
  if (s.bookOpening) return WaitReason::BookOpening;
  if (!deadlineReached(s.nowMs, s.nextAttemptMs)) return WaitReason::Backoff;
  if (s.wakePending && !s.homeShown) return WaitReason::Home;
  return WaitReason::NextEvent;
}

}  // namespace reader_sync
