#pragma once

#include <Benchmark.h>
#include <NavRestore.h>

class GfxRenderer;
class MappedInputManager;

// On-device navigation benchmark: drives a table of semantic steps (lib/Benchmark/NavScript.h) by injecting real
// physical button presses through HalGPIO, observing the current activity and display refreshes, and recording
// nav_step / nav_result rows into the same automatic session format as the page-turn run.
//
// The run is owned here, not by an activity, so it survives the screens it navigates through. tick() is called from
// the main loop before MappedInputManager::update().
//
// A run opens books and toggles settings, so it works on a copy of the owner's state: start() snapshots recents,
// reading progress, statistics and settings to the card, and the run restores them when it ends, diverges, is
// interrupted, or (after a crash or sleep) at the next boot via recover(). A failed restore keeps the backup, is
// logged and shown on the Benchmark screen, and blocks the next run until a retry succeeds.
namespace nav_run {

enum class StartResult : uint8_t {
  Started,
  Busy,
  ScriptTooLarge,
  BadScript,
  RecorderFailed,
  OutOfMemory,
  BackupFailed,   // the state snapshot could not be written; nothing was run
  RestorePending  // an earlier run's backup could not be restored yet
};

struct Status {
  bool ran = false;        // a run has started since boot
  bool running = false;
  bool finalized = false;  // the session footer was written and closed
  bool customScript = false;
  bool restoreRan = false;     // the post-run restore was attempted
  bool restoreFailed = false;  // it was not complete: recents, positions or statistics may differ
  benchmark::nav::RestoreResult restore{};
  benchmark::nav::Outcome outcome = benchmark::nav::Outcome::Complete;
  benchmark::nav::Summary summary{};
  char path[80]{};  // this run's own session file
};

// Where an optional runtime script is read from; absent means the built-in table.
constexpr const char* SCRIPT_PATH = "/.crosspoint/benchmark/nav-script.txt";
constexpr size_t SCRIPT_MAX = 24576;

StartResult start(GfxRenderer& renderer, MappedInputManager& input);
// Main task, once per loop, before MappedInputManager::update().
void tick();
bool active();
const Status& status();

// Early in setup(), once the card is mounted and before the stores load: puts back the state of a run that never
// restored (crash, power loss, sleep). Logs the result; a failure blocks new runs until a later retry succeeds.
void recover();
// True while an earlier run's state could not be restored.
bool restorePending();

// Shared by the page-turn run and this one: hardware and reader settings rows of a fresh session.
void captureEnvironment();

}  // namespace nav_run
