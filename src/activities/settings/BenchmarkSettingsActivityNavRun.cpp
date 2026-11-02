#include "BenchmarkSettingsActivityNavRun.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <WiFi.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <NavDefaultScript.h>
#include <NavFeatures.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "BenchmarkSettingsActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "ReadingStats.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "network/ReaderSync.h"
#include "WifiCredentialStore.h"

namespace nav = benchmark::nav;

namespace {

constexpr uint32_t RETURN_TIMEOUT_MS = 5000;
// After the script ends the run waits for a quiet Settings screen before it rewrites the card.
constexpr uint32_t SETTLE_TIMEOUT_MS = 10000;

struct Run {
  nav::Sequencer sequencer;
  std::unique_ptr<char[]> loaded;  // runtime script text; null when the built-in table is used
  GfxRenderer* renderer = nullptr;
  MappedInputManager* input = nullptr;
  bool active = false;
  bool resumeNormal = false;
  bool returnPending = false;
  uint32_t returnStartedMs = 0;
  // The script ended; waiting for a quiet Settings screen, then restore, then close the session.
  bool restoring = false;
  bool settingsRequested = false;
  uint32_t restoreStartedMs = 0;
  nav::Summary pendingSummary{};
  bool backupTaken = false;
  // Software actions only touch RAM; these are put back verbatim after the run.
  uint8_t savedMenuStyle = 0;
  char savedDictionary[sizeof(CrossPointSettings::dictionaryName)]{};
  // A real offline Reader sync attempt started by the current `sync` step, until its worker reaches a terminal phase.
  bool syncAwaiting = false;
  uint32_t syncRunsBefore = 0;
};

Run run;
nav_run::Status current;
bool restoreStillPending = false;

bool credentialsAbsent() { return WIFI_STORE.getCredentialCount() == 0; }

// No Wi-Fi interface running and not connected (the Wi-Fi screens stop the station when they are left).
bool radioQuiet() { return WiFi.getMode() == WIFI_MODE_NULL && !WiFi.isConnected(); }

// No path to a network exists: no credential in the in-memory store and the radio is off.
bool networkIdle() { return credentialsAbsent() && radioQuiet(); }

// The device side of the pure feature table: what is on the card, in the stores and on this board.
class DeviceProbe final : public nav::FeatureProbe {
 public:
  bool exists(const char* path) const override { return Storage.exists(path); }
  uint32_t recentCount() const override { return static_cast<uint32_t>(RECENT_BOOKS.getCount()); }
  bool dictionarySelected() const override { return SETTINGS.dictionaryName[0] != '\0'; }
  bool touch() const override { return gpio.hasTouch(); }
  bool opdsConfigured() const override { return OPDS_STORE.hasServers(); }
  bool networkIdle() const override { return ::networkIdle(); }
};

// Re-checks the safety conditions at the moment of the press, then lets the real scheduler launch its worker for this
// one step. False (the run diverges) when a condition no longer holds: nothing is permitted then.
bool beginRealSync() {
  const auto status = reader_sync::status();
  const bool idle = networkIdle();
  const bool wifiFile = Storage.exists("/.crosspoint/wifi.json");
  if (!idle || wifiFile || !status.configured || status.active) {
    LOG_ERR("BENCH", "real reader sync refused: idle=%d wifi_file=%d configured=%d active=%d", idle ? 1 : 0,
            wifiFile ? 1 : 0, status.configured ? 1 : 0, status.active ? 1 : 0);
    return false;
  }
  run.syncRunsBefore = status.finishedRuns;
  run.syncAwaiting = true;
  nav::setSyncPermit(true);
  LOG_INF("BENCH", "real reader sync permitted (offline, no credentials), runs=%u",
          static_cast<unsigned>(status.finishedRuns));
  return true;
}

// While the permitted attempt runs: stop at once if a network path appears, and read the terminal phase when it ends.
// Only Offline is expected; anything else means the attempt was not the offline one the guard promised.
void watchRealSync() {
  if (!(credentialsAbsent() && radioQuiet())) {
    LOG_ERR("BENCH", "real reader sync: a network path appeared; cancelling the worker");
    reader_sync::cancelAndJoin();
    run.syncAwaiting = false;
    nav::setSyncPermit(false);
    run.sequencer.abort(nav::Outcome::Diverged);
    return;
  }
  const auto status = reader_sync::status();
  if (status.finishedRuns == run.syncRunsBefore) return;
  run.syncAwaiting = false;
  nav::setSyncPermit(false);
  LOG_INF("BENCH", "real reader sync finished: phase=%u runs=%u", static_cast<unsigned>(status.phase),
          static_cast<unsigned>(status.finishedRuns));
  if (status.phase != reader_sync::Phase::Offline) run.sequencer.abort(nav::Outcome::Diverged);
}

bool deviceFeature(const uint32_t id, void*) {
  static const DeviceProbe probe;
  return nav::featureAvailable(probe, id);
}

// The card as the pure backup layer sees it. FAT cannot rename over an existing file, so a file is replaced through a
// temporary name and nav::replaceFile (previous content moved aside, never deleted first), which the host tests cover.
class DeviceFs final : public nav::RestoreFs {
 public:
  bool exists(const char* path) override { return Storage.exists(path); }

  bool fileSize(const char* path, uint32_t& size) override {
    HalFile file = Storage.open(path, O_RDONLY);
    if (!file || file.isDirectory()) return false;
    size = static_cast<uint32_t>(file.size());
    return true;
  }

  bool readAt(const char* path, uint32_t offset, uint8_t* buf, size_t cap, size_t& n) override {
    n = 0;
    HalFile file = Storage.open(path, O_RDONLY);
    if (!file || file.isDirectory()) return false;
    if (offset != 0 && !file.seekSet(offset)) return false;
    const int got = cap == 0 ? 0 : file.read(buf, cap);
    if (got < 0) return false;
    n = static_cast<size_t>(got);
    return true;
  }

  bool writeFile(const char* path, const uint8_t* data, size_t n) override {
    const std::string target(path);
    const size_t slash = target.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && !Storage.ensureDirectoryExists(target.substr(0, slash).c_str())) {
      return false;
    }
    const std::string temporary = target + ".nbtmp";
    {
      HalFile file = Storage.open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
      if (!file) return false;
      const bool written = n == 0 || file.write(data, n) == n;
      const bool synced = written && file.sync();
      const bool closed = file.close();
      if (!synced || !closed) {
        Storage.remove(temporary.c_str());
        return false;
      }
    }
    if (!nav::replaceFile(*this, temporary.c_str(), path)) {
      Storage.remove(temporary.c_str());
      return false;
    }
    return true;
  }

  bool appendFile(const char* path, const uint8_t* data, size_t n) override {
    HalFile file = Storage.open(path, O_WRONLY | O_CREAT | O_APPEND);
    if (!file) return false;
    const bool written = n == 0 || file.write(data, n) == n;
    const bool synced = written && file.sync();
    return file.close() && synced;
  }

  bool removeFile(const char* path) override { return Storage.remove(path); }

  // Plain rename: fails when `to` exists (SdFat), deletes nothing.
  bool renameFile(const char* from, const char* to) override { return Storage.rename(from, to); }

  bool makeDir(const char* path) override { return Storage.ensureDirectoryExists(path); }

  bool listDir(const char* dir, const std::function<bool(const char* name, bool isDir)>& visit) override {
    HalFile directory = Storage.open(dir, O_RDONLY);
    if (!directory || !directory.isDirectory()) return false;
    char name[256];
    for (HalFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
      entry.getName(name, sizeof(name));
      const bool isDir = entry.isDirectory();
      entry.close();
      if (!visit(name, isDir)) break;
    }
    return true;
  }
};

// Mirrors MappedInputManager::mapButton for every logical key the script grammar can name. -1: the user's button
// layout has no physical button for it (side buttons disabled), so the run stops instead of pressing something else.
int physicalFor(const nav::Key key, const MappedInputManager& input) {
  const bool swapped = input.isNavDirectionSwapped();
  switch (key) {
    case nav::Key::Back: return SETTINGS.frontButtonBack;
    case nav::Key::Confirm: return SETTINGS.frontButtonConfirm;
    case nav::Key::Left: return SETTINGS.frontButtonLeft;
    case nav::Key::Right: return SETTINGS.frontButtonRight;
    case nav::Key::Up: return HalGPIO::BTN_UP;
    case nav::Key::Down: return HalGPIO::BTN_DOWN;
    case nav::Key::PageBack:
      switch (SETTINGS.sideButtonLayout) {
        case CrossPointSettings::PREV_NEXT: return swapped ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
        case CrossPointSettings::NEXT_PREV: return swapped ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
        case CrossPointSettings::PREV_PREV: return HalGPIO::BTN_UP;
        default: return -1;
      }
    case nav::Key::PageForward:
      switch (SETTINGS.sideButtonLayout) {
        case CrossPointSettings::PREV_NEXT: return swapped ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
        case CrossPointSettings::NEXT_PREV: return swapped ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
        case CrossPointSettings::NEXT_NEXT: return HalGPIO::BTN_DOWN;
        default: return -1;
      }
  }
  return -1;
}

// Software actions (`do=<verb>:<arg>`): the same calls the screens make, never a file write. false = the step cannot
// be performed (missing book, name too long) and the run stops as diverged.
bool performAction(const nav::Command& command) {
  switch (command.action) {
    case nav::ActionVerb::Open:
      if (!Storage.exists(command.arg)) {
        LOG_ERR("BENCH", "do=open: %s is not on the card", command.arg);
        return false;
      }
      activityManager.goToReader(command.arg);
      return true;
    case nav::ActionVerb::Dictionary:
      if (strlen(command.arg) >= sizeof(SETTINGS.dictionaryName)) return false;
      snprintf(SETTINGS.dictionaryName, sizeof(SETTINGS.dictionaryName), "%s", command.arg);
      return true;
    case nav::ActionVerb::MenuStyle:
      SETTINGS.readerMenuStyle = strcmp(command.arg, "toolbar") == 0 ? CrossPointSettings::READER_MENU_TOOLBAR
                                                                      : CrossPointSettings::READER_MENU_LIST;
      return true;
    case nav::ActionVerb::Goto:
      // The parser admits only "library" and "home". Both are Replace transitions that clear the activity stack. Home opens
      // with the Settings row selected, the state a Back out of the Settings screen leaves, whatever Settings tab or cursor
      // the run was started from.
      if (strcmp(command.arg, "home") == 0) {
        activityManager.goHome(HomeMenuItem::SETTINGS_MENU);
      } else {
        activityManager.goToLibrary();
      }
      return true;
    case nav::ActionVerb::Idle: return true;  // the quiet/norefresh window after it is the measurement
    case nav::ActionVerb::None: break;
  }
  return false;
}

// Re-reads every store the run or the restore may have changed behind its RAM copy. The two RAM-only action
// settings are put back from the values captured before the run: a settings file without those keys would
// otherwise keep the script's value.
void reloadState(const uint8_t menuStyle, const char* dictionary) {
  SETTINGS.loadFromFile();
  SETTINGS.readerMenuStyle = menuStyle;
  snprintf(SETTINGS.dictionaryName, sizeof(SETTINGS.dictionaryName), "%s", dictionary);
  RECENT_BOOKS.loadFromFile();
  APP_STATE.loadFromFile();
  reading_stats::reload();
}

bool restoreOk(const nav::RestoreResult& result) { return result.found && result.ok(); }

void logRestore(const char* when, const nav::RestoreResult& result) {
  if (restoreOk(result)) {
    LOG_INF("BENCH", "%s: restored %u files, removed %u, swept %u extras", when, static_cast<unsigned>(result.restored),
            static_cast<unsigned>(result.removed), static_cast<unsigned>(result.extrasRemoved));
  } else {
    LOG_ERR("BENCH", "%s: RESTORE FAILED (found=%d valid=%d failed=%u of %u, first path hash %08x); backup kept",
            when, result.found, result.valid, static_cast<unsigned>(result.failed),
            static_cast<unsigned>(result.attempted), static_cast<unsigned>(result.firstFailedHash));
  }
}

// Settles an earlier run's backup: restores it when a complete manifest is on the card. True when nothing is
// left unrestored. `reload` re-reads the stores afterwards (the boot path runs before they are loaded).
bool settleStaleBackup(const bool reload, const char* when) {
  DeviceFs fs;
  if (!nav::backupPending(fs)) {
    restoreStillPending = false;
    return true;
  }
  const uint8_t menuStyle = SETTINGS.readerMenuStyle;
  char dictionary[sizeof(SETTINGS.dictionaryName)];
  snprintf(dictionary, sizeof(dictionary), "%s", SETTINGS.dictionaryName);
  const nav::RestoreResult result = nav::restoreSnapshot(fs);
  logRestore(when, result);
  restoreStillPending = !restoreOk(result);
  if (reload && !restoreStillPending) reloadState(menuStyle, dictionary);
  return !restoreStillPending;
}

void resumeNormalRecording() {
  if (!run.resumeNormal) return;
  run.resumeNormal = false;
  if (benchmark::begin(benchmark::SessionMode::Normal)) nav_run::captureEnvironment();
}

// Ends the session (footer, sync, close) and releases the device. Main task, RenderLock held by the caller.
void closeSession(const nav::Summary& summary, const bool restoreFailed) {
  benchmark::flush();
  benchmark::record(nav::encodeResult(summary, benchmark::nowUs()));
  // Complete only when nothing failed: a guard/expect divergence, timeout, recorder fault or unrestored state never
  // closes the log with a successful footer. A user interrupt and a cancel are Cancelled.
  using Outcome = nav::Outcome;
  const bool success = summary.outcome == Outcome::Complete && summary.failed == 0 && !restoreFailed;
  const auto reason = success ? benchmark::StopReason::Complete
                      : (summary.outcome == Outcome::Cancelled || summary.outcome == Outcome::Interrupted) &&
                              !restoreFailed
                          ? benchmark::StopReason::Cancelled
                          : benchmark::StopReason::Failure;
  current.finalized = benchmark::stop(reason);
  nav::disarmProbe();
  gpio.setInjectionGuard(false);
  nav::setRunActive(false);
  current.running = false;
  current.outcome = summary.outcome;
  current.summary = summary;
  run.active = false;
  run.restoring = false;
  run.loaded.reset();
  run.syncAwaiting = false;
  nav::setSyncPermit(false);
  resumeNormalRecording();
}

// Puts the owner's state back, records nav_restore, closes the session and returns to the Benchmark menu.
void restoreAndClose() {
  {
    RenderLock lock;
    const uint64_t startedUs = benchmark::nowUs();
    nav::RestoreResult result;
    bool failed = false;
    if (run.backupTaken) {
      DeviceFs fs;
      result = nav::restoreSnapshot(fs);
      failed = !restoreOk(result);
      logRestore("run end", result);
    }
    restoreStillPending = failed;
    // Reload even without a snapshot restore: a diverged run may have changed RAM that was saved nowhere.
    reloadState(run.savedMenuStyle, run.savedDictionary);

    benchmark::Event event;
    event.type = benchmark::EventType::NavRestore;
    event.startUs = startedUs;
    event.endUs = benchmark::nowUs();
    event.data[0] = failed ? 0 : 1;
    event.data[1] = result.attempted;
    event.data[2] = result.restored;
    event.data[3] = result.removed;
    event.data[4] = result.extrasRemoved;
    event.data[5] = result.failed;
    event.data[6] = result.firstFailedHash;
    event.data[7] = result.found ? 1 : 0;
    benchmark::record(event);

    current.restoreRan = run.backupTaken;
    current.restoreFailed = failed;
    current.restore = result;
    run.backupTaken = false;
    closeSession(run.pendingSummary, failed);
  }
  // Back to the Benchmark menu, where the result is listed: Settings first, then the Benchmark screen on top.
  if (activityManager.currentActivityId() != benchmark::id("BenchmarkSettings")) {
    if (activityManager.currentActivityId() != benchmark::id("Settings")) activityManager.goToSettings();
    run.returnPending = true;
    run.returnStartedMs = millis();
  }
}

// The script ended (or stopped). Nothing may write the card while a reader is still saving its progress, so the run
// goes to Settings and waits for it to be quiet before restoring.
void tickRestore() {
  if (gpio.injectionInFlight()) return;
  const bool timedOut = millis() - run.restoreStartedMs > SETTLE_TIMEOUT_MS;
  const bool atSettings = activityManager.currentActivityId() == benchmark::id("Settings");
  if (!atSettings && !run.settingsRequested) {
    activityManager.goToSettings();
    run.settingsRequested = true;
    return;
  }
  const bool quiet = atSettings && !activityManager.hasPendingWork() && !RenderLock::peek();
  if (!quiet && !timedOut) return;
  if (timedOut && !quiet) {
    LOG_ERR("BENCH", "Settings did not go quiet in %u ms; restoring anyway", static_cast<unsigned>(SETTLE_TIMEOUT_MS));
  }
  restoreAndClose();
}

void finish(const nav::Summary& summary) {
  run.pendingSummary = summary;
  run.restoring = true;
  run.settingsRequested = false;
  run.restoreStartedMs = millis();
}

void pushBenchmarkScreen() {
  run.returnPending = false;
  if (auto activity = makeUniqueNoThrow<BenchmarkSettingsActivity>(*run.renderer, *run.input)) {
    activityManager.pushActivity(std::move(activity));
  } else {
    LOG_ERR("BENCH", "OOM: Benchmark screen after navigation run");
  }
}

}  // namespace

namespace nav_run {

void captureEnvironment() {
  benchmark::hardware();
  benchmark::settings(SETTINGS.fontFamily, SETTINGS.fontPointSize, SETTINGS.lineSpacing, SETTINGS.orientation,
                      SETTINGS.textAntiAliasing, SETTINGS.screenInverted, SETTINGS.screenMargin, SETTINGS.sleepScreen,
                      benchmark::id(SETTINGS.sdFontFamilyName));
}

bool active() { return run.active; }
const Status& status() { return current; }
bool restorePending() { return restoreStillPending; }

void recover() { settleStaleBackup(false, "boot recovery"); }

StartResult start(GfxRenderer& renderer, MappedInputManager& input) {
  if (run.active) return StartResult::Busy;
  reader_sync::cancelAndJoin();
  // A backup left by an earlier run is put back first; while that fails nothing runs on top of it.
  if (!settleStaleBackup(true, "before run")) return StartResult::RestorePending;
  const char* script = nav::DEFAULT_SCRIPT;
  bool custom = false;
  run.loaded.reset();
  if (Storage.exists(SCRIPT_PATH)) {
    // The buffer is sized to the file, so a normal run (no script on the card) allocates nothing and a 12 KB script
    // holds 12 KB for the run. A file over the cap is rejected from its size, never truncated or read.
    size_t fileBytes = 0;
    {
      HalFile scriptFile = Storage.open(SCRIPT_PATH, O_RDONLY);
      if (!scriptFile) return StartResult::BadScript;
      fileBytes = scriptFile.fileSize();
    }
    if (fileBytes > SCRIPT_MAX) return StartResult::ScriptTooLarge;
    const auto heapBefore = HalMemory::getInternalHeap();
    auto text = makeUniqueNoThrow<char[]>(fileBytes + 2);
    if (!text) return StartResult::OutOfMemory;
    const size_t bytes = Storage.readFileToBuffer(SCRIPT_PATH, text.get(), fileBytes + 2, 0);
    const auto heapAfter = HalMemory::getInternalHeap();
    // The recorded cost of a script: internal free bytes before and after its buffer, and the size of the largest block.
    LOG_INF("BENCH", "nav script %u bytes; internal free %u -> %u, largest block %u -> %u", static_cast<unsigned>(bytes),
            static_cast<unsigned>(heapBefore.freeBytes), static_cast<unsigned>(heapAfter.freeBytes),
            static_cast<unsigned>(heapBefore.largestBlockBytes), static_cast<unsigned>(heapAfter.largestBlockBytes));
    if (bytes > SCRIPT_MAX) return StartResult::ScriptTooLarge;
    script = text.get();
    custom = true;
    run.loaded = std::move(text);
  }

  RenderLock lock;
  run.resumeNormal = SETTINGS.benchmarkRecord || benchmark::enabled();
  if (benchmark::enabled()) benchmark::stop(benchmark::StopReason::User);
  if (!benchmark::begin(benchmark::SessionMode::Automatic) || !benchmark::enabled()) {
    resumeNormalRecording();
    run.loaded.reset();
    return StartResult::RecorderFailed;
  }
  captureEnvironment();
  current = Status{};
  current.ran = true;
  current.customScript = custom;
  // Copy before any later begin() replaces the recorder's latest path.
  snprintf(current.path, sizeof(current.path), "%s", benchmark::sessionPath());

  run.renderer = &renderer;
  run.input = &input;
  run.returnPending = false;
  run.restoring = false;
  if (!run.sequencer.begin(script, benchmark::nowUs(), &deviceFeature, nullptr)) {
    // Invalid script: the result row carries the error line and code, then the session closes as a failure.
    benchmark::record(nav::encodeResult(run.sequencer.summary(), benchmark::nowUs()));
    current.finalized = benchmark::stop(benchmark::StopReason::Failure);
    current.outcome = nav::Outcome::BadScript;
    current.summary = run.sequencer.summary();
    resumeNormalRecording();
    run.loaded.reset();
    return StartResult::BadScript;
  }

  // Snapshot last: only a run that will really start leaves a backup behind.
  {
    DeviceFs fs;
    const nav::SnapshotResult snapshot = nav::snapshot(fs);
    if (!snapshot.ok()) {
      LOG_ERR("BENCH", "state backup failed (error %u, path hash %08x); nothing was run",
              static_cast<unsigned>(snapshot.error), static_cast<unsigned>(snapshot.failedHash));
      nav::Summary summary = run.sequencer.summary();
      summary.outcome = nav::Outcome::Recorder;
      benchmark::record(nav::encodeResult(summary, benchmark::nowUs()));
      current.finalized = benchmark::stop(benchmark::StopReason::Failure);
      current.outcome = nav::Outcome::Recorder;
      current.summary = summary;
      resumeNormalRecording();
      run.loaded.reset();
      return snapshot.error == nav::SnapshotError::Pending ? StartResult::RestorePending : StartResult::BackupFailed;
    }
    run.backupTaken = true;
    LOG_INF("BENCH", "state backed up: %u files recorded, %u present", static_cast<unsigned>(snapshot.entries),
            static_cast<unsigned>(snapshot.present));
  }
  run.savedMenuStyle = SETTINGS.readerMenuStyle;
  snprintf(run.savedDictionary, sizeof(run.savedDictionary), "%s", SETTINGS.dictionaryName);

  nav::setRunActive(true);
  gpio.setInjectionGuard(true);
  nav::armProbe(benchmark::nowUs());
  run.active = true;
  current.running = true;
  return StartResult::Started;
}

void tick() {
  if (!run.active) {
    if (run.returnPending) {
      if (activityManager.currentActivityId() == benchmark::id("Settings")) {
        pushBenchmarkScreen();
      } else if (millis() - run.returnStartedMs > RETURN_TIMEOUT_MS) {
        run.returnPending = false;
      }
    }
    return;
  }
  if (run.restoring) {
    tickRestore();
    return;
  }
  // The script advances only between presses; a held injected press finishes (and is released) first.
  if (gpio.injectionInFlight()) return;
  if (run.syncAwaiting) watchRealSync();
  if (gpio.realInputSeen()) run.sequencer.abort(nav::Outcome::Interrupted);
  else if (!benchmark::enabled()) run.sequencer.abort(nav::Outcome::Recorder);

  const auto heap = HalMemory::getInternalHeap();
  nav::Observation observation;
  observation.nowUs = benchmark::nowUs();
  observation.activityId = activityManager.currentActivityId();
  observation.busy = activityManager.hasPendingWork() || RenderLock::peek();
  observation.heapFree = static_cast<uint32_t>(heap.freeBytes);
  observation.heapMinEver = static_cast<uint32_t>(heap.minFreeBytes);
  observation.refresh = nav::probeSnapshot();
  observation.syncPending = run.syncAwaiting;

  nav::Command command;
  while (run.sequencer.poll(observation, command)) {
    switch (command.kind) {
      case nav::Command::Kind::Inject: {
        const int button = physicalFor(command.press.key, *run.input);
        if (button < 0) {
          run.sequencer.abort(nav::Outcome::Unmappable);
          break;
        }
        if ((command.stepFlags & nav::FlagSync) && !beginRealSync()) {
          run.sequencer.abort(nav::Outcome::Diverged);
          break;
        }
        nav::armProbe(command.armAtUs);
        if (!gpio.injectPress(static_cast<uint8_t>(button), command.press.holdMs)) {
          // Not reachable behind the in-flight gate above; stop rather than lose a press silently.
          run.sequencer.abort(nav::Outcome::Recorder);
          break;
        }
        // The edge reaches the activity at the next MappedInputManager::update(); observe again next loop.
        return;
      }
      case nav::Command::Kind::Act: {
        nav::armProbe(command.armAtUs);
        if (!performAction(command)) {
          run.sequencer.abort(nav::Outcome::Diverged);
          break;
        }
        // Navigation queued by the action is observed on the next loop, like a press.
        return;
      }
      case nav::Command::Kind::Record: {
        RenderLock lock;
        benchmark::record(nav::encodeStep(command.record));
        benchmark::flush();
        break;
      }
      case nav::Command::Kind::Finish:
        finish(command.summary);
        return;
      case nav::Command::Kind::None: break;
    }
  }
}

}  // namespace nav_run
