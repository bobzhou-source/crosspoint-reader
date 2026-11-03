#pragma once

#include <cstddef>
#include <cstdint>

namespace benchmark {

enum class SessionMode : uint8_t { Normal, Automatic };
enum class EventType : uint8_t {
  Activity, Render, Display, Busy, Memory, Index, Phase, Input, Sleep, Loss, Flush, End, ReaderSettings, DisplaySettings,
  RunStep, RunResult, SyncRun, Battery, RefreshClear, ManualRefresh
};
enum class StopReason : uint8_t {
  Sleep = 0, UsbDrive = 1, Restart = 2, Limit = 3, User = 4, Complete = 5, Cancelled = 6, Failure = 7
};
enum class DisplayCall : uint8_t {
  Buffer, AsyncSubmit, AsyncWait, Refresh, GrayBase, Gray, Precondition, Sleep, ConfiguredGrayBase, RegionClear
};
// Phase rows carry a detail slot (data[3]). Prerender (13) is the idle EPUB page prerender: its detail is a
// page_prerender::Outcome. On a page render, detail 1 marks a page presented from a prerendered body.
enum class Phase : uint8_t { Prewarm, BwRender, Display, GrayRender, Wait, GrayWrite, GrayDisplay, Cleanup, BwStore,
                             GrayLsb, GrayMsb, BwRestore, Total, Prerender };

struct Event {
  uint64_t startUs = 0;
  uint64_t endUs = 0;
  uint32_t sequence = 0;
  EventType type = EventType::Activity;
  uint32_t data[8] = {};
  // refresh_clear / manual_refresh only: the logical rect a region clear
  // covered (x, y, w, h), all zero for a whole-screen clear.
  uint16_t region[4] = {};
};

// Synchronization belongs to the caller; no allocation, strings, or I/O on push.
class Buffer {
 public:
  static constexpr size_t CAPACITY = 64;
  // Discards queued events without touching storage; the next session starts at sequence 1.
  void reset() {
    first = 0;
    count = 0;
    nextSequence = 1;
    lost = 0;
  }
  void push(const Event& event);
  bool pop(Event& event);
  uint32_t dropped() const { return lost; }
  size_t size() const { return count; }

 private:
  Event events[CAPACITY]{};
  size_t first = 0;
  size_t count = 0;
  uint32_t nextSequence = 1;
  uint32_t lost = 0;
};

// Fixed output budget includes a reserved terminal record; never rotates/deletes files.
class Writer {
 public:
  static constexpr size_t MAX_BYTES = 1024 * 1024;
  static constexpr size_t TERMINAL_RESERVE = 1024;
  enum class Result { Written, Limit, Failed };
  using Write = size_t (*)(void*, const char*, size_t);
  void begin(Write write, void* context);
  Result line(const char* text, size_t length, bool terminal = false);
  Result event(const Event& event, bool terminal = false);
  void disable() { live = false; }
  bool active() const { return live; }
  size_t bytes() const { return written; }

 private:
  Write sink = nullptr;
  void* context = nullptr;
  size_t written = 0;
  bool live = false;
  char encoded[512]{};
};

// Stable, non-cryptographic IDs, never persisted source strings or paths.
uint32_t id(const char* text);
// Returns zero if the complete escaped string (including quotes) will not fit.
size_t jsonString(char* output, size_t capacity, const char* input);

}  // namespace benchmark
