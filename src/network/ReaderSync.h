#pragma once

#include "ReaderSyncProtocol.h"
class GfxRenderer;

namespace reader_sync {
struct Status {
  Phase phase = Phase::Unconfigured;
  bool configured = false;
  bool enabled = false;
  bool active = false;
  uint32_t revision = 0;
  uint32_t books = 0;
  uint32_t notes = 0;
  uint32_t rejectedNotes = 0;
  int64_t lastSuccess = 0;
  Schedule schedule = Schedule::Events;
  WaitReason waitReason = WaitReason::Unconfigured;
  uint32_t waitMinutes = 0;
  bool manualPending = false;
};
// What the activity layer knows about the foreground screen (see ScheduleState).
struct UiState {
  bool permitted = false;    // healthy normal UI, not a benchmark workload
  bool homeShown = false;    // Home is the foreground activity
  bool bookOpening = false;  // reader is foreground but has not rendered its first page
};
// Render-task hook: atomic store only, never I/O or task creation.
void healthyRender();
// Main-loop hooks, outside RenderLock and storage locks.
void tick(GfxRenderer& renderer, const UiState& ui);
void cancelAndJoin();
// Deep-sleep hook (main task, sleep screen already shown, SD still mounted). Runs one
// bounded sync when annotation uploads are pending; always returns with the radio off,
// after at most PRE_SLEEP_BUDGET_MS plus the time the worker needs to unwind. Returns
// true when a sync was attempted.
bool syncBeforeSleep(GfxRenderer& renderer);
// AnnotationStore hook: a note was written and has not been uploaded yet.
void annotationSaved();
// onExit may hold RenderLock: defer radio cleanup to the transition's unlocked boundary.
void deferSelectionRadioRelease();
void finishActivityTransition();
void syncNow();
bool setEnabled(bool enabled);
// Persists the schedule in reader-sync.json (writes the optional "schedule" key).
bool setSchedule(Schedule schedule);
Status status();
}  // namespace reader_sync
