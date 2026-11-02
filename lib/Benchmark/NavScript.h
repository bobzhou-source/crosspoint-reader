#pragma once

// Navigation benchmark script: pure parsing, sequencing and record encoding. No Arduino/ESP/HAL types, so the whole
// layer is covered by host tests. The device glue (input injection, activity/heap/display observation) lives in
// src/activities/settings/BenchmarkSettingsActivityNavRun.*.
//
// Script text, one step per line ('#' starts a comment, blank lines ignored):
//
//   <step-id> guard=<Activity|any> [lead=<keys>] [key=<key>] [x=<reps>] [expect=<Activity>] [needs=<feature>]
//             [quiet=<ms>] [norefresh=<ms>] [timeout=<ms>] [do=<verb>:<arg>] [setup] [soft] [optional] [external]
//
// - step-id: stable semantic name shared with the emulator runner (e.g. home.library.open). Unique per script,
//   except that variants of one step for different prerequisites (each with its own `needs=`) may share an id.
// - guard: activity name that MUST be current before anything is injected, or `any` to opt out. A step whose guard
//   is not met FAILS the run (status Diverged, nothing injected): a fixed button stream never runs blind, and a
//   supported step is never silently skipped. Only an `optional` step is skipped (status Unsupported) instead.
// - lead: comma-separated unmeasured presses injected one by one, each waiting for the screen to settle first.
// - key: the measured press. `x=N` repeats it N times; each repetition is its own record.
// - keys: Back Confirm Left Right Up Down PageForward PageBack, optionally `@<holdMs>` and (lead only) `*<count>`.
// - expect: activity name required once the step has settled. A mismatch ends the run (Diverged) unless `soft`.
// - needs: feature the device must report available; otherwise the step is skipped (status Skipped).
// - quiet: ms without a display refresh before the screen counts as settled (default 700).
// - norefresh: ms after the press with no refresh at all before "settled without refresh" (default 2500).
// - timeout: ms after which the step gives up (default 120000).
// - setup: perform the presses but write no nav_step row.
// - optional: the step depends on a prerequisite that may legitimately be absent (a fixture book, a recents entry);
//   an unmet guard then skips the step as Unsupported instead of failing the run.
// - external: a step the device cannot perform (sleep/wake); recorded as External, never injected.
// - do: a software action instead of `key` (measured the same way): `open:<path>` opens a book exactly as a
//   Library/Files/Home Confirm does, `dict:<folder>` selects the dictionary under /dictionaries and
//   `menu:<list|toolbar>` the reader menu style. The two settings change in memory only; the files on the card are
//   rewritten only if the run itself saves settings, and are restored after the run. Leads may precede it; a step
//   has a key or an action. `idle:<label>` does nothing (the window is the measurement: an index finishing, a
//   background task settling).
// - blind: the step's screen state cannot be named by an activity guard (an overlay inside one activity). A settled
//   blind step is recorded as Unverified, never Ok: its timing is real, its landing screen is not proven.

#include <cstddef>
#include <cstdint>

#include "BenchmarkBuffer.h"

namespace benchmark::nav {

// Logical keys. The device maps them to physical buttons through the user's button layout.
enum class Key : uint8_t { Back, Confirm, Left, Right, Up, Down, PageForward, PageBack };

struct Press {
  Key key = Key::Confirm;
  uint16_t holdMs = 60;
};

enum StepFlag : uint8_t { FlagSetup = 1, FlagSoft = 2, FlagExternal = 4, FlagOptional = 8, FlagBlind = 16, FlagSync = 32 };

enum class ActionVerb : uint8_t { None = 0, Open = 1, Dictionary = 2, MenuStyle = 3, Idle = 4, Goto = 5 };

struct Step {
  static constexpr size_t ID_MAX = 40;
  static constexpr size_t TOKEN_MAX = 28;
  static constexpr size_t LEAD_MAX = 16;
  static constexpr size_t ARG_CHARS = 64;
  char id[ID_MAX]{};
  uint32_t idHash = 0;
  bool anyGuard = false;
  uint32_t guardId = 0;
  bool hasExpect = false;
  uint32_t expectId = 0;
  bool hasNeeds = false;
  uint32_t needsId = 0;
  Press lead[LEAD_MAX]{};
  uint8_t leadCount = 0;
  bool hasKey = false;
  Press key{};
  ActionVerb action = ActionVerb::None;
  char arg[ARG_CHARS]{};
  uint8_t reps = 1;
  uint16_t quietMs = 700;
  uint16_t noRefreshMs = 2500;
  uint32_t timeoutMs = 120000;
  uint8_t flags = 0;
};

enum class ParseCode : uint8_t {
  Ok,
  BadStepId,
  DuplicateId,
  UnknownField,
  UnknownKey,
  BadNumber,
  MissingGuard,
  TooManyLead,
  TokenTooLong,
  NoAction,
  NoSteps,
  TooManySteps,
  BadAction
};

struct ParseError {
  ParseCode code = ParseCode::Ok;
  uint32_t line = 0;  // 1-based line in the script text; 0 when the script as a whole is wrong
};

constexpr size_t MAX_STEPS = 400;

// Parses one line. Returns Ok with `blank` set for comments/empty lines.
ParseCode parseLine(const char* begin, const char* end, Step& step, bool& blank);
// Validates the whole script (no allocation) and counts its steps.
ParseError validate(const char* script, size_t& stepCount);
const char* parseCodeName(ParseCode code);

// Refresh evidence gathered by the display hook since the probe was armed. Attribution is by time only: a refresh
// caused by something other than the injected press (a clock tick, background render) is counted too, so a step
// reports every refresh that landed in its window, never a guess about causality.
enum class RefreshCall : uint8_t { None = 0, Buffer = 1, Refresh = 2, AsyncWait = 3, GrayBase = 4, Gray = 5, ConfiguredGrayBase = 6 };

struct RefreshSnapshot {
  uint32_t count = 0;
  uint64_t firstStartUs = 0;  // when the first counted display call began (submission, not completion)
  uint64_t lastEndUs = 0;
  RefreshCall firstCall = RefreshCall::None;
  uint8_t firstMode = 0;
  RefreshCall lastCall = RefreshCall::None;
  uint8_t lastMode = 0;
};

class RefreshProbe {
 public:
  // Starts a fresh window at `atUs`. An async submit made before this point is forgotten: its completion is not
  // attributed to the new window.
  void arm(uint64_t atUs);
  // `call` is a benchmark::DisplayCall value; `mode` its slot-1 argument; `startUs` / `endUs` the call's begin and
  // completion times (software, not optical).
  void onDisplayCall(uint8_t call, uint32_t mode, uint64_t startUs, uint64_t endUs);
  RefreshSnapshot snapshot() const { return current; }
  uint64_t armedAtUs() const { return armedAt; }

 private:
  void count(RefreshCall call, uint32_t mode, uint64_t startUs, uint64_t endUs);
  RefreshSnapshot current{};
  uint64_t armedAt = 0;
  bool asyncPending = false;
  uint8_t asyncMode = 0;
  uint64_t asyncStartUs = 0;
};

enum class StepStatus : uint8_t {
  Ok = 0,           // a refresh was observed and the screen settled
  NoRefresh = 1,    // the press produced no refresh; the screen was quiet for norefresh ms
  Timeout = 2,      // never settled in time; the run ends
  Unsupported = 3,  // optional step whose guard activity is not current; nothing was injected
  Diverged = 4,     // settled in another activity than expected
  Skipped = 5,      // needed feature not available on this device/build
  External = 6,     // sleep/wake style step the device cannot self-drive
  Unverified = 7    // a `blind` step settled: timing recorded, landing screen not proven by a guard
};

// Unmappable: the user's button layout cannot produce a key the step needs. Interrupted: a real button/touch
// edge arrived mid-run and the run yielded to the user.
enum class Outcome : uint8_t {
  Complete = 0, Cancelled = 1, Timeout = 2, Diverged = 3, BadScript = 4, Recorder = 5, Unmappable = 6, Interrupted = 7
};

struct Observation {
  uint64_t nowUs = 0;
  uint32_t activityId = 0;  // benchmark::id(activity name)
  bool busy = false;        // a render or index is still in flight
  uint32_t heapFree = 0;    // current internal free bytes
  uint32_t heapMinEver = 0; // internal minimum-ever free bytes
  RefreshSnapshot refresh{};
  // A permitted real Reader sync attempt started by the current `sync` step has not reached a terminal status yet.
  bool syncPending = false;
};

struct StepRecord {
  uint32_t stepIndex = 0;  // 1-based, counting every step line including skipped ones
  uint32_t rep = 0;        // 0-based repetition
  uint32_t idHash = 0;
  StepStatus status = StepStatus::Ok;
  uint64_t injectUs = 0;
  uint64_t settledUs = 0;      // end of the last refresh, or the press time when there was none
  uint64_t firstSubmitUs = 0;  // when the first refresh call began (not its completion); zero when no refresh
  uint32_t refreshCount = 0;
  RefreshCall firstCall = RefreshCall::None;
  uint8_t firstMode = 0;
  RefreshCall lastCall = RefreshCall::None;
  uint8_t lastMode = 0;
  uint32_t heapMinEver = 0;
  uint32_t heapMinSampled = 0;  // lowest internal free bytes seen while the step ran
};

struct Summary {
  Outcome outcome = Outcome::Complete;
  uint32_t steps = 0;       // step lines visited
  uint32_t measured = 0;    // nav_step rows with a real press
  uint32_t ok = 0;
  uint32_t noRefresh = 0;
  uint32_t unverified = 0;  // blind steps that settled; not counted as ok
  uint32_t skipped = 0;     // Unsupported + Skipped: not applicable here, never counted as passed
  uint32_t external = 0;    // External steps the device cannot self-drive
  uint32_t failed = 0;      // Timeout + Diverged
  uint32_t elapsedMs = 0;
  uint32_t scriptId = 0;
  ParseError error{};
};

struct Command {
  enum class Kind : uint8_t { None, Inject, Act, Record, Finish };
  Kind kind = Kind::None;
  Press press{};        // Inject: arm the probe at `armAtUs`, then press
  ActionVerb action = ActionVerb::None;  // Act: arm the probe at `armAtUs`, then perform the action
  uint8_t stepFlags = 0;                 // Inject/Act/Record: the step's StepFlag bits
  char arg[Step::ARG_CHARS]{};
  uint64_t armAtUs = 0;
  StepRecord record{};  // Record
  Summary summary{};    // Finish
};

using FeatureCheck = bool (*)(uint32_t featureId, void* context);

class Sequencer {
 public:
  // Validates the script and starts at its first step. False (with summary().error set) when it is invalid.
  // `script` must outlive the sequencer.
  bool begin(const char* script, uint64_t nowUs, FeatureCheck features, void* featureContext);
  // Produces at most one command per call; call again while it returns true.
  bool poll(const Observation& observation, Command& out);
  // The next poll finishes the run as Cancelled (or `outcome` for abort()).
  void cancel() { abort(Outcome::Cancelled); }
  void abort(Outcome outcome) {
    cancelled = true;
    abortOutcome = outcome;
  }
  bool finished() const { return state == State::Done; }
  const Summary& summary() const { return result; }

 private:
  enum class State : uint8_t { Idle, LoadStep, Lead, AwaitLead, Measure, AwaitMeasure, Finishing, Done };

  bool loadStep();
  bool settled(const Observation& observation, const Step& step, uint64_t sinceUs) const;
  bool finish(const Observation& observation, Outcome outcome, Command& out);
  bool skip(const Observation& observation, StepStatus status, Command& out);

  const char* script = nullptr;
  const char* cursor = nullptr;
  FeatureCheck features = nullptr;
  void* featureContext = nullptr;
  State state = State::Idle;
  bool cancelled = false;
  Outcome abortOutcome = Outcome::Cancelled;
  Step step{};
  uint32_t stepIndex = 0;
  uint8_t leadDone = 0;
  uint8_t repDone = 0;
  uint64_t injectUs = 0;
  uint64_t startUs = 0;
  uint32_t heapMinSampled = 0;
  Outcome pendingOutcome = Outcome::Complete;
  Summary result{};
};

// Encoders for the append-only event kinds. Slot layouts are documented in USER_GUIDE.md.
Event encodeStep(const StepRecord& record);
Event encodeResult(const Summary& summary, uint64_t nowUs);
// Generic input marker for the main input poll, every activity: flags for any press / release / touch, whether the
// edge was injected by a navigation run, and the hashed current activity name.
Event encodeInput(bool pressed, bool released, bool touch, bool injected, uint32_t activityId, uint64_t nowUs);

}  // namespace benchmark::nav
