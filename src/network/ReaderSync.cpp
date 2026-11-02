#include "ReaderSync.h"

#include <Arduino.h>
#include <Benchmark.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <ctime>

#include "HttpDownloader.h"
#include "WifiConnection.h"
#include "WifiCredentialStore.h"
#include "WifiDiagnostics.h"
#include "activities/RenderLock.h"
#include "util/BatterySample.h"
#include "util/BookCacheUtils.h"

namespace reader_sync {
namespace {
std::atomic<bool> healthy{false};
std::atomic<bool> active{false};
std::atomic<bool> stop{false};
std::atomic<bool> manual{false};
std::atomic<bool> releaseSelectionRadio{false};
// Boot and wake are the same event: the chip resets out of deep sleep, so this
// starts true on every power-up. Cleared by the first uncancelled run or by a
// debounced wake evaluation.
std::atomic<bool> wakePending{true};
// The last uncancelled run reported more bounded work (books or notes).
std::atomic<bool> continuation{false};
// Wall-clock time of the last uncancelled run, successful or not (0 = never/unknown).
std::atomic<int64_t> lastAttemptUtc{0};
// Annotations written since the last clean upload pass. Mirrored on the SD card by
// PENDING_PATH so it survives a reboot or power loss. Generation counts saves.
std::atomic<bool> pendingNotes{false};
std::atomic<uint32_t> noteGeneration{0};
std::mutex statusMutex;
Status snapshot;
Config config;
TaskHandle_t worker = nullptr;
bool loaded = false;
uint32_t nextAttempt = 0;
std::string after;
bool parkedUntilWake = false;
constexpr char STATUS_PATH[] = "/.crosspoint/reader-sync/status.json";
constexpr char PENDING_PATH[] = "/.crosspoint/reader-sync/notes.pending";
constexpr uint32_t EXTERNAL_POWER_POLL_MS = 5000;

// Context handed to the worker for one run (written before the notify).
Trigger runTrigger = Trigger::None;
battery_sample::Sample runBefore;
uint32_t runGeneration = 0;

// What the worker measured. Written before `active` falls; consumed on the main task.
struct RunLog {
  Trigger trigger = Trigger::None;
  Phase phase = Phase::Offline;
  uint32_t durationMs = 0;
  uint32_t finishedMs = 0;
  uint32_t bytesSent = 0;
  uint32_t bytesReceived = 0;
  uint32_t books = 0;
  uint32_t notes = 0;
  uint32_t generation = 0;
  bool notesClean = false;
  bool radioOff = true;
  battery_sample::Sample before;
};
RunLog runLog;
std::atomic<bool> runLogReady{false};

void phase(Phase phase) {
  std::lock_guard<std::mutex> guard(statusMutex);
  snapshot.phase = phase;
  snapshot.active = active.load();
  ++snapshot.revision;
}
bool aborted(void*) {
  // Long readback and directory scans must yield even when the SD is fast.
  delay(1);
  return stop.load();
}
int64_t utc() { return static_cast<int64_t>(time(nullptr)); }
void randomBytes(uint8_t* bytes, size_t size) { esp_fill_random(bytes, size); }

bool connectSaved(bool& radioStarted) {
  radioStarted = false;
  auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!credential) credential = WIFI_STORE.getCredentialAt(0);
  if (!credential || stop.load() || WiFi.getMode() != WIFI_MODE_NULL) return false;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  const bool ready = WiFi.mode(WIFI_STA);
  // mode() can fail after starting the interface; retain ownership for cleanup
  // if a started mode is visible. No credentials or pre-start cancellation owns nothing.
  radioStarted = WiFi.getMode() != WIFI_MODE_NULL;
  if (!ready || !radioStarted || stop.load()) return false;
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  uint8_t mac[6]{};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
    char hostname[40];
    snprintf(hostname, sizeof(hostname), "CrossPoint-Reader-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    WiFi.setHostname(hostname);
  }
  WiFi.setSleep(true);
  wifi_connection::startDiagnostics(credential->ssid);
  if (!wifi_connection::begin(*credential)) {
    wifi_connection::finishDiagnostics("configuration-failed");
    return false;
  }
  const uint32_t started = millis();
  const uint32_t timeoutMs = credential->enterprise ? 30000 : 12000;
  while (!stop.load() && millis() - started < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
      wifi_connection::finishDiagnostics("connected");
      return true;
    }
    delay(100);
  }
  wifi_connection::finishDiagnostics(stop.load() ? "cancelled" : "timeout");
  return false;
}

// OFF stops/deinitializes our STA without erasing SDK NVS or saved credentials.
// Never touches an interface this attempt did not start. Returns true when the
// attempt leaves no radio running.
bool releaseRadio(const bool radioStarted) {
  if (!radioStarted) return true;
  wifi_connection::clearEnterprise();
  WiFi.mode(WIFI_OFF);
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    // mode() can fail while a disconnect is still in flight: drop the association, retry once.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  return WiFi.getMode() == WIFI_MODE_NULL;
}

void persistStatus() {
  const Status state = status();
  JsonDocument doc;
  doc["last_success"] = state.lastSuccess;
  doc["books"] = state.books;
  doc["notes"] = state.notes;
  doc["last_attempt"] = lastAttemptUtc.load();
  std::string raw;
  serializeJson(doc, raw);
  if (!doc.overflowed()) atomicWrite(STATUS_PATH, reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
}
void runWorker(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    {
      HalPowerManager::Lock power;
      const uint32_t startedMs = millis();
      Result result;
      result.phase = Phase::Offline;
      bool radioStarted = false;
      phase(Phase::Connecting);
      if (connectSaved(radioStarted) && !stop.load()) {
        if (!halClock.syncFromNTP(aborted, nullptr) || utc() < MIN_VALID_UTC) result.phase = Phase::ClockError;
        else if (!stop.load()) {
          const auto heap = HalMemory::getInternalHeap();
          if (heap.freeBytes < HttpDownloader::MIN_TLS_FREE_HEAP || heap.largestBlockBytes < HttpDownloader::MIN_TLS_MAX_ALLOC) {
            result.phase = Phase::MemoryError;
          } else {
            phase(Phase::Syncing);
            auto protocol = makeUniqueNoThrow<Protocol>(config, aborted, nullptr, utc, randomBytes, clearBookCache);
            if (protocol) result = protocol->run(after);
            else result.phase = Phase::MemoryError;
          }
        }
      }
      // Protocol/TLS/file RAII has completed before the radio is released.
      const bool radioOff = releaseRadio(radioStarted);
      if (stop.load()) result.phase = Phase::Cancelled;
      const bool finished = result.phase == Phase::Success || result.phase == Phase::AnnotationError;
      after = finished ? result.after : "";
      parkedUntilWake = result.parkUntilWake;
      uint32_t seconds = config.intervalSeconds;
      if (finished && result.more) seconds = std::max<uint32_t>(60U, result.retrySeconds);
      if (result.phase == Phase::RateLimited) seconds = std::max<uint32_t>(60U, result.retrySeconds);
      if (result.phase == Phase::Cancelled) seconds = 60;
      nextAttempt = millis() + seconds * 1000U;
      if (result.phase != Phase::Cancelled) {
        // Any completed attempt satisfies the wake event; an offline device is not
        // retried until the next wake, charge interval or manual sync.
        wakePending.store(false);
        continuation.store(finished && result.more);
        const int64_t now = utc();
        if (now >= MIN_VALID_UTC) lastAttemptUtc.store(now);
      }
      {
        std::lock_guard<std::mutex> guard(statusMutex);
        snapshot.books += result.books;
        snapshot.notes += result.notes;
        snapshot.rejectedNotes = result.rejectedNotes;
        if (finished) snapshot.lastSuccess = utc();
        snapshot.phase = result.phase;
        snapshot.active = false;
        ++snapshot.revision;
      }
      if (!stop.load()) persistStatus();
      runLog = RunLog{};
      runLog.trigger = runTrigger;
      runLog.phase = result.phase;
      runLog.finishedMs = millis();
      runLog.durationMs = runLog.finishedMs - startedMs;
      runLog.bytesSent = result.bytesSent;
      runLog.bytesReceived = result.bytesReceived;
      runLog.books = result.books;
      runLog.notes = result.notes;
      runLog.generation = runGeneration;
      runLog.notesClean = finished && !result.more;
      runLog.radioOff = radioOff;
      runLog.before = runBefore;
      runLogReady.store(true, std::memory_order_release);
    }
    // Release is last: cancelAndJoin may now detach SD or enter a foreground radio owner.
    active.store(false, std::memory_order_release);
  }
}

void clearPendingNotes() {
  if (!Storage.ready()) return;
  if (Storage.exists(PENDING_PATH) && !Storage.remove(PENDING_PATH)) return;
  pendingNotes.store(false, std::memory_order_release);
}

// Main task. Emits the sync log line and session-log event for the last run, with the
// battery read after the radio is already off, and settles the pending-upload marker.
void flushRunLog() {
  if (!runLogReady.exchange(false, std::memory_order_acq_rel)) return;
  const RunLog log = runLog;
  const battery_sample::Sample batteryAfter = battery_sample::read();
  LOG_INF("RSYNC", "run trigger=%s phase=%u ms=%u rx=%u tx=%u books=%u notes=%u mv=%u>%u pct=%u>%u radio_off=%d",
          triggerName(log.trigger), static_cast<unsigned>(log.phase), static_cast<unsigned>(log.durationMs),
          static_cast<unsigned>(log.bytesReceived), static_cast<unsigned>(log.bytesSent),
          static_cast<unsigned>(log.books), static_cast<unsigned>(log.notes),
          static_cast<unsigned>(log.before.millivolts), static_cast<unsigned>(batteryAfter.millivolts),
          static_cast<unsigned>(log.before.percent), static_cast<unsigned>(batteryAfter.percent),
          log.radioOff ? 1 : 0);
  if (!log.radioOff) LOG_ERR("RSYNC", "radio still on after sync");
  if (log.notesClean && noteGeneration.load(std::memory_order_acquire) == log.generation) clearPendingNotes();
  if (!benchmark::enabled()) return;
  benchmark::Event event;
  event.type = benchmark::EventType::SyncRun;
  // The worker finished before this flush ran; place the span where the run really was.
  const uint64_t nowUs = benchmark::nowUs();
  const uint64_t sinceFinishUs = static_cast<uint64_t>(static_cast<uint32_t>(millis() - log.finishedMs)) * 1000ULL;
  const uint64_t durationUs = static_cast<uint64_t>(log.durationMs) * 1000ULL;
  event.endUs = nowUs > sinceFinishUs ? nowUs - sinceFinishUs : nowUs;
  event.startUs = event.endUs > durationUs ? event.endUs - durationUs : 0;
  event.data[0] = static_cast<uint32_t>(log.trigger);
  event.data[1] = static_cast<uint32_t>(log.phase) | (log.radioOff ? 0U : 0x100U);
  event.data[2] = log.bytesReceived;
  event.data[3] = log.bytesSent;
  event.data[4] = log.before.millivolts;
  event.data[5] = batteryAfter.millivolts;
  event.data[6] = log.before.percent;
  event.data[7] = batteryAfter.percent;
  benchmark::record(event);
}

bool externalPower() {
  static bool polled = false;
  static bool cached = false;
  static uint32_t polledMs = 0;
  const uint32_t now = millis();
  if (!polled || now - polledMs >= EXTERNAL_POWER_POLL_MS) {
    cached = gpio.isUsbConnected();
    polled = true;
    polledMs = now;
  }
  return cached;
}

ScheduleState scheduleState(const UiState& ui, const bool sleeping) {
  const Status state = status();
  ScheduleState s;
  s.schedule = config.schedule;
  s.configured = state.configured;
  s.enabled = state.enabled;
  s.manualRequested = manual.load();
  s.workerActive = active.load(std::memory_order_acquire);
  s.radioForeground = WiFi.getMode() != WIFI_MODE_NULL;  // Foreground Wi-Fi still owns the radio.
  s.uiPermitted = ui.permitted;
  s.homeShown = ui.homeShown;
  s.bookOpening = ui.bookOpening;
  s.sleeping = sleeping;
  s.wakePending = wakePending.load();
  s.continuation = continuation.load();
  s.pendingUploads = pendingNotes.load(std::memory_order_acquire);
  s.parkedUntilWake = parkedUntilWake;
  s.rateLimited = state.phase == Phase::RateLimited;
  s.nowMs = millis();
  s.nextAttemptMs = nextAttempt;
  s.intervalSeconds = config.intervalSeconds;
  const int64_t now = utc();
  s.lastSuccessAgeS = ageSeconds(now, state.lastSuccess);
  s.lastAttemptAgeS = ageSeconds(now, lastAttemptUtc.load());
  // Only Events mode consults power, so Interval mode never touches the gauge.
  if (!sleeping && s.enabled && ui.permitted && config.schedule == Schedule::Events) s.externalPower = externalPower();
  return s;
}

void load() {
  loaded = true;
  const bool exists = Storage.exists(CONFIG_PATH) || Storage.exists("/.crosspoint/reader-sync.json.bak");
  const bool configured = loadConfig(config);
  {
    std::lock_guard<std::mutex> guard(statusMutex);
    snapshot.configured = configured;
    snapshot.enabled = configured && config.enabled;
    snapshot.schedule = config.schedule;
    snapshot.phase = configured ? (config.enabled ? Phase::Waiting : Phase::Disabled) :
                                 (exists ? Phase::InvalidConfig : Phase::Unconfigured);
    ++snapshot.revision;
  }
  if (!configured) return;
  pendingNotes.store(Storage.exists(PENDING_PATH), std::memory_order_release);
  auto file = Storage.open(STATUS_PATH);
  if (file && file.size() <= 1024) {
    char raw[256];
    const int count = file.read(raw, sizeof(raw));
    JsonDocument doc;
    if (count > 0 && !deserializeJson(doc, raw, count)) {
      std::lock_guard<std::mutex> guard(statusMutex);
      snapshot.lastSuccess = doc["last_success"] | int64_t{0};
      snapshot.books = doc["books"] | uint32_t{0};
      snapshot.notes = doc["notes"] | uint32_t{0};
      lastAttemptUtc.store(doc["last_attempt"] | int64_t{0});
    }
  }
}

// Main task. Starts one worker run; false when the render lock or the task is unavailable.
bool launch(GfxRenderer& renderer, const Trigger trigger) {
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) return false;
    if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
  }
  if (!worker && xTaskCreate(runWorker, "ReaderSync", 12288, nullptr, 1, &worker) != pdPASS) {
    worker = nullptr;
    nextAttempt = millis() + config.intervalSeconds * 1000U;
    manual.store(false);
    phase(Phase::MemoryError);
    return false;
  }
  manual.store(false);
  stop.store(false);
  runTrigger = trigger;
  runBefore = battery_sample::read();  // Radio still off: an unloaded reading.
  runGeneration = noteGeneration.load(std::memory_order_acquire);
  active.store(true, std::memory_order_release);
  xTaskNotifyGive(worker);
  return true;
}
}  // namespace

void healthyRender() { healthy.store(true, std::memory_order_release); }
Status status() { std::lock_guard<std::mutex> guard(statusMutex); return snapshot; }

void cancelAndJoin() {
  manual.store(false);
  stop.store(true);
  // Only the worker closes its handles. Killing it would strand SdFat's mutex.
  while (active.load(std::memory_order_acquire)) delay(10);
}
void deferSelectionRadioRelease() { releaseSelectionRadio.store(true); }
void finishActivityTransition() {
  if (!releaseSelectionRadio.exchange(false) || WiFi.getMode() == WIFI_MODE_NULL) return;
  releaseRadio(true);
}
void syncNow() { manual.store(true); }
bool setEnabled(bool enabled) {
  cancelAndJoin();
  if (!loaded) load();
  if (!status().configured) return false;
  if (config.enabled == enabled) return true;
  config.enabled = enabled;
  if (!saveConfig(config)) { config.enabled = !enabled; phase(Phase::StorageError); return false; }
  {
    std::lock_guard<std::mutex> guard(statusMutex);
    snapshot.enabled = enabled;
  }
  phase(enabled ? Phase::Waiting : Phase::Disabled);
  if (enabled) manual.store(true);
  return true;
}

bool setSchedule(const Schedule schedule) {
  cancelAndJoin();
  if (!loaded) load();
  if (!status().configured) return false;
  if (config.schedule == schedule && config.scheduleExplicit) return true;
  const Config previous = config;
  config.schedule = schedule;
  config.scheduleExplicit = true;
  if (!saveConfig(config)) { config = previous; phase(Phase::StorageError); return false; }
  {
    std::lock_guard<std::mutex> guard(statusMutex);
    snapshot.schedule = schedule;
  }
  // A deliberate switch must not fire a stale deadline: the next periodic run is one interval away.
  nextAttempt = millis() + config.intervalSeconds * 1000U;
  phase(config.enabled ? Phase::Waiting : Phase::Disabled);
  return true;
}

void annotationSaved() {
  noteGeneration.fetch_add(1, std::memory_order_acq_rel);
  pendingNotes.store(true, std::memory_order_release);
  // The marker only matters when the service is configured; do not create the private
  // tree for devices that never set it up.
  if (!status().configured || !Storage.ready() || !Storage.ensureDirectoryExists(PRIVATE_ROOT)) return;
  auto marker = Storage.open(PENDING_PATH, O_WRONLY | O_CREAT);
  if (!marker || !marker.close()) LOG_ERR("RSYNC", "could not record pending annotation upload");
}

void tick(GfxRenderer& renderer, const UiState& ui) {
  flushRunLog();
  if (!healthy.load(std::memory_order_acquire) || !Storage.ready()) return;
  if (!loaded) load();
  if (active.load(std::memory_order_acquire)) return;
  const ScheduleState scheduled = scheduleState(ui, false);
  const Decision decision = decide(scheduled);
  const WaitReason reason = explainDecision(scheduled, decision, WIFI_STORE.getCredentialCount() != 0);
  const uint32_t minutes =
      reason == WaitReason::Backoff ? (scheduled.nextAttemptMs - scheduled.nowMs + 59999) / 60000 : 0;
  {
    std::lock_guard<std::mutex> guard(statusMutex);
    if (snapshot.waitReason != reason || snapshot.waitMinutes != minutes ||
        snapshot.manualPending != scheduled.manualRequested) {
      snapshot.waitReason = reason;
      snapshot.waitMinutes = minutes;
      snapshot.manualPending = scheduled.manualRequested;
      ++snapshot.revision;
    }
  }
  if (!ui.permitted) return;
  if (decision.consumeWake) wakePending.store(false);
  if (decision.trigger == Trigger::None) return;
  launch(renderer, decision.trigger);
}

bool syncBeforeSleep(GfxRenderer& renderer) {
  cancelAndJoin();
  flushRunLog();
  if (!healthy.load(std::memory_order_acquire) || !Storage.ready()) return false;
  if (!loaded) load();
  if (decide(scheduleState(UiState{}, true)).trigger != Trigger::PreSleep) return false;
  if (!launch(renderer, Trigger::PreSleep)) return false;
  const uint32_t startedMs = millis();
  while (active.load(std::memory_order_acquire) && millis() - startedMs < PRE_SLEEP_BUDGET_MS) delay(10);
  const bool timedOut = active.load(std::memory_order_acquire);
  if (timedOut) {
    // Sleep is never blocked past the budget: the worker unwinds at its next abort check
    // and releases the radio itself, so join here before the caller tears anything down.
    stop.store(true);
    while (active.load(std::memory_order_acquire)) delay(10);
  }
  flushRunLog();
  if (timedOut) {
    // A cancelled run records no attempt time; without this every sleep would wait the
    // full budget again on a slow or unreachable server.
    const int64_t now = utc();
    if (now >= MIN_VALID_UTC) lastAttemptUtc.store(now);
    persistStatus();
    LOG_ERR("RSYNC", "pre-sleep sync stopped at the %u ms budget", static_cast<unsigned>(PRE_SLEEP_BUDGET_MS));
  }
  return true;
}
}  // namespace reader_sync
