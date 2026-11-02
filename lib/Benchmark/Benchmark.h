#pragma once

#include "BenchmarkBuffer.h"
#include "NavScript.h"

namespace benchmark {

// Cache the boot wake reason and register the shutdown handler once. Creates no SD files.
void initialize(uint32_t wakeReason);
// Starts a new session; false when one is already active or setup failed.
bool begin(SessionMode mode);
// Path of the latest created session. Copy it before starting another session.
const char* sessionPath();
void hardware();  // After runtime panel detection.
bool enabled();
uint64_t nowUs();
uint64_t start();  // Zero and no clock read when disabled.
void record(const Event& event);
void point(EventType type, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0);
void span(EventType type, uint64_t started, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0);
void intervalMs(EventType type, uint32_t startMs, uint32_t endMs, uint32_t code = 0, uint32_t detail = 0);
void memory();

namespace nav {
// True while a navigation run owns the device (set by the run, read by ActivityManager and main). Atomic.
void setRunActive(bool active);
bool runActive();
// While set, and only while the foreground activity is the Reader Sync screen, the Reader sync scheduler may launch
// its worker although a run is active. Set by the run for one guarded `sync` step and cleared when that step ends.
void setSyncPermit(bool permitted);
bool syncPermit();
// Arms the display-call probe at `atUs` (benchmark::nowUs() clock) and starts observing completed refreshes.
// Only calls recorded while a session is active are seen. Safe from any task.
void armProbe(uint64_t atUs);
void disarmProbe();
RefreshSnapshot probeSnapshot();
}  // namespace nav

// ReaderSettings slot 4 records AdaptiveRefreshPolicy::kVersion.
void settings(uint32_t font, uint32_t pointSize, uint32_t spacing, uint32_t orientation, uint32_t antialias,
              uint32_t inverted, uint32_t margin, uint32_t sleepScreen, uint32_t sdFontId);
// Main task only, with RenderLock actually owned (not a peek). Never from a measurement.
bool flushDue();
void flush();
// Serializes with flush, disables producers, drains and closes before SD ownership changes.
// True only when the active session finalized (footer, sync, close); false when inactive or failed.
// Also installed as the ESP normal shutdown handler; never used from a panic/ISR.
bool stop(StopReason reason);

// For synchronous viewers which paint during onEnter(), outside the render task.
class RenderSpan {
 public:
  explicit RenderSpan(const char* activity) : begun(start()), activityId(begun ? id(activity) : 0) {}
  ~RenderSpan() { span(EventType::Render, begun, activityId, completed, 2); }
  void complete() { completed = true; }

 private:
  uint64_t begun;
  uint32_t activityId;
  bool completed = false;
};

class DisplaySpan {
 public:
  explicit DisplaySpan(DisplayCall call, uint32_t mode = 0, uint32_t detail = 0)
      : begun(start()), call(call), mode(mode), detail(detail) {}
  ~DisplaySpan() { span(EventType::Display, begun, static_cast<uint32_t>(call), mode, detail); }

 private:
  uint64_t begun;
  DisplayCall call;
  uint32_t mode;
  uint32_t detail;
};

}  // namespace benchmark
