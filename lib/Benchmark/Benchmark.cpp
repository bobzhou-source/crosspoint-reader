#include "Benchmark.h"

#include <AdaptiveRefreshPolicy.h>
#include <Arduino.h>
#include <BoardConfig.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <XteinkDetect.h>
#include <esp_app_desc.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace benchmark {
namespace {
std::atomic<bool> recording{false};
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;
StaticSemaphore_t ioMutexStorage;
SemaphoreHandle_t ioMutex = nullptr;
Buffer buffer;
Writer writer;
HalFile file;
// Shared scratch is only touched under ioMutex; no large task-stack buffers.
char metadata[1024];
char escaped[160];
char imageHash[65];
std::atomic<uint32_t> lastFlushMs{0};
uint64_t lastMemoryUs = 0;
uint32_t reportedLost = 0;
uint32_t bootWakeReason = 0;
bool shutdownRegistered = false;
char sessionPathStorage[80]{};
// Navigation-run refresh evidence. Display calls are observed only while a probe is armed.
portMUX_TYPE probeMux = portMUX_INITIALIZER_UNLOCKED;
nav::RefreshProbe refreshProbe;
std::atomic<bool> probeActive{false};
uint32_t previousSettings[10]{};
bool haveSettings = false;

class IoLock {
 public:
  IoLock() { xSemaphoreTake(ioMutex, portMAX_DELAY); }
  ~IoLock() { xSemaphoreGive(ioMutex); }
};

size_t writeBytes(void*, const char* bytes, size_t count) { return file.write(bytes, count); }

void disableLocked() {
  recording.store(false, std::memory_order_relaxed);
  writer.disable();
  if (file) file.close();  // Member handle must close before SD unmount/raw ownership.
  file = HalFile{};
}

void failLocked() {
  LOG_ERR("BENCH", "SD recorder disabled after file/write/sync failure");
  disableLocked();
}

bool metadataLocked(const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int count = vsnprintf(metadata, sizeof(metadata), format, args);
  va_end(args);
  if (count < 0 || static_cast<size_t>(count) >= sizeof(metadata) ||
      writer.line(metadata, static_cast<size_t>(count)) != Writer::Result::Written) {
    failLocked();
    return false;
  }
  return true;
}

bool pop(Event& event) {
  taskENTER_CRITICAL(&queueMux);
  const bool result = buffer.pop(event);
  taskEXIT_CRITICAL(&queueMux);
  return result;
}

uint32_t lostCount() {
  taskENTER_CRITICAL(&queueMux);
  const uint32_t result = buffer.dropped();
  taskEXIT_CRITICAL(&queueMux);
  return result;
}

// The observer row measures the batch's write+sync, not its own persistence or close.
// False means the writer or session was disabled; a closing drain then has no valid footer.
// True for a closing drain also requires the requested stop reason: a writer cap that forced a
// Limit footer truncated the capture, so an explicit Complete/User/Cancelled stop reports failure.
bool drainLocked(bool closing, StopReason reason) {
  const StopReason requested = reason;
  bool truncated = false;
  const uint64_t begun = nowUs();
  const size_t before = writer.bytes();
  uint32_t discarded = 0;
  Event event;
  // Bounded even if another task is still producing. Normal flush holds RenderLock.
  for (size_t i = 0; i < Buffer::CAPACITY && pop(event); ++i) {
    const auto result = writer.event(event);
    if (result == Writer::Result::Failed) {
      failLocked();
      return false;
    }
    if (result == Writer::Result::Limit) {
      recording.store(false, std::memory_order_relaxed);
      closing = true;
      reason = StopReason::Limit;
      truncated = true;
      discarded = 1;
      while (pop(event)) ++discarded;
      break;
    }
  }
  const uint32_t lost = lostCount();
  if (lost != reportedLost) {
    event = {};
    event.type = EventType::Loss;
    event.startUs = event.endUs = nowUs();
    event.data[0] = lost;
    // Reserve is sufficient for loss, flush and end rows together.
    const auto result = writer.event(event, true);
    if (result != Writer::Result::Written) {
      failLocked();
      return false;
    }
    reportedLost = lost;
  }
  if (!file.sync()) {
    failLocked();
    return false;
  }
  event = {};
  event.type = EventType::Flush;
  event.startUs = begun;
  event.endUs = nowUs();
  event.data[0] = static_cast<uint32_t>(writer.bytes() - before);
  event.data[1] = static_cast<uint32_t>(writer.bytes());
  auto result = writer.event(event, closing);
  if (result == Writer::Result::Limit) {
    recording.store(false, std::memory_order_relaxed);
    closing = true;
    reason = StopReason::Limit;
    truncated = true;
    while (pop(event)) ++discarded;
    event = {};
    event.type = EventType::Flush;
    event.startUs = begun;
    event.endUs = nowUs();
    event.data[0] = static_cast<uint32_t>(writer.bytes() - before);
    event.data[1] = static_cast<uint32_t>(writer.bytes());
    result = writer.event(event, true);
  }
  if (result != Writer::Result::Written) {
    failLocked();
    return false;
  }
  if (closing) {
    event = {};
    event.type = EventType::End;
    event.startUs = event.endUs = nowUs();
    event.data[0] = static_cast<uint32_t>(reason);
    event.data[1] = lost;
    event.data[2] = discarded;
    event.data[3] = static_cast<uint32_t>(writer.bytes());
    if (writer.event(event, true) != Writer::Result::Written) {
      failLocked();
      return false;
    }
  }
  if (!file.sync()) {
    failLocked();
    return false;
  }
  lastFlushMs.store(millis(), std::memory_order_relaxed);
  if (!closing) return !truncated;
  const bool closed = file.close();
  if (!closed) LOG_ERR("BENCH", "Recorder close failed; log tail may be incomplete");
  disableLocked();
  return closed && (!truncated || requested == StopReason::Limit);
}

void shutdown() { stop(StopReason::Restart); }
}  // namespace

bool enabled() { return recording.load(std::memory_order_relaxed); }
uint64_t nowUs() { return static_cast<uint64_t>(esp_timer_get_time()); }
uint64_t start() { return enabled() ? nowUs() : 0; }

void initialize(uint32_t wakeReason) {
  bootWakeReason = wakeReason;
  if (!ioMutex) {
    ioMutex = xSemaphoreCreateMutexStatic(&ioMutexStorage);
    if (!ioMutex) {
      LOG_ERR("BENCH", "Recorder mutex unavailable");
      return;
    }
  }
  if (!shutdownRegistered) {
    shutdownRegistered = esp_register_shutdown_handler(shutdown) == ESP_OK;
    if (!shutdownRegistered) LOG_ERR("BENCH", "Recorder shutdown handler unavailable");
  }
}

bool begin(SessionMode mode) {
  if (!ioMutex) {
    LOG_ERR("BENCH", "Recorder not initialized");
    return false;
  }
  IoLock lock;
  if (recording.load(std::memory_order_relaxed)) return false;
  if (!Storage.ready()) {
    LOG_ERR("BENCH", "SD unavailable; benchmark session not started");
    return false;
  }
  // A new session never inherits queued rows, loss counts, sequence numbers or settings dedupe.
  taskENTER_CRITICAL(&queueMux);
  buffer.reset();
  haveSettings = false;
  memset(previousSettings, 0, sizeof(previousSettings));
  taskEXIT_CRITICAL(&queueMux);
  reportedLost = 0;
  lastMemoryUs = 0;
  lastFlushMs.store(millis(), std::memory_order_relaxed);
  writer.begin(writeBytes, nullptr);
  if (!Storage.ensureDirectoryExists("/Benchmark/logs")) {
    LOG_ERR("BENCH", "Benchmark log directory unavailable");
    disableLocked();
    return false;
  }
  char path[80]{};
  // Bounded collision search plus O_EXCL: never truncate even with a repeated RNG seed.
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    snprintf(path, sizeof(path), "/Benchmark/logs/session-%08lx-%08lx-%02u.jsonl",
             static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()), attempt);
    if (Storage.exists(path)) continue;
    file = Storage.open(path, O_WRONLY | O_CREAT | O_EXCL);
    break;
  }
  if (!file) {
    LOG_ERR("BENCH", "Could not create benchmark log");
    disableLocked();
    return false;
  }
  memcpy(sessionPathStorage, path, sizeof(sessionPathStorage));
  const auto* app = esp_app_get_description();
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; ++i) {
    imageHash[2 * i] = HEX_DIGITS[app->app_elf_sha256[i] >> 4];
    imageHash[2 * i + 1] = HEX_DIGITS[app->app_elf_sha256[i] & 15];
  }
  imageHash[64] = '\0';
  const char* modeName = mode == SessionMode::Automatic ? "automatic" : "normal";
  if (!jsonString(escaped, sizeof(escaped), CROSSPOINT_VERSION) ||
      !metadataLocked("{\"event\":\"session\",\"schema\":3,\"mode\":\"%s\",\"version\":%s,\"app_elf_sha256\":\"%s\","
                      "\"build_date\":\"%s\",\"build_time\":\"%s\",\"boot_us\":%llu,\"reset_reason\":%u,"
                      "\"wake_cause\":%u,\"hal_wake_reason\":%u,\"max_bytes\":%u,\"buffer_events\":%u,"
                      "\"phase_resolution_us\":1000,\"clock\":\"esp_timer_since_boot\","
                      "\"memory_unit\":\"bytes\",\"index_phase_clock_unit\":\"ms\",\"id_algorithm\":\"fnv1a32\","
                      "\"refresh_policy\":\"%s\"}\n",
                      modeName, escaped, imageHash, app->date, app->time,
                      static_cast<unsigned long long>(nowUs()), static_cast<unsigned>(esp_reset_reason()),
                      static_cast<unsigned>(esp_sleep_get_wakeup_cause()), static_cast<unsigned>(bootWakeReason),
                      static_cast<unsigned>(Writer::MAX_BYTES), static_cast<unsigned>(Buffer::CAPACITY),
                      AdaptiveRefreshPolicy::kName)) {
    return false;  // metadataLocked disabled the session.
  }
  if (!file.sync()) {
    failLocked();
    return false;
  }
  recording.store(true, std::memory_order_relaxed);
  memory();
  return true;
}

const char* sessionPath() { return sessionPathStorage; }

void hardware() {
  if (!enabled()) return;
  IoLock lock;
  if (!enabled()) return;
  const auto& probe = freeink::getXteinkDisplayProbeDiag();
  if (!jsonString(escaped, sizeof(escaped), BoardConfig::ACTIVE.name)) {
    failLocked();
    return;
  }
  metadataLocked("{\"event\":\"hardware\",\"board\":%s,\"controller\":%u,\"variant\":%u,"
                 "\"width_px\":%u,\"height_px\":%u,\"probe_valid\":%u,\"probe_verdict\":%u,"
                 "\"probe_promoted\":%u,\"probe_busy_timeout\":%u,\"probe_ver\":[%u,%u,%u,%u,%u],"
                 "\"probe_ver_bytes\":%u,\"probe_flg\":%u}\n",
                 escaped, static_cast<unsigned>(BoardConfig::ACTIVE.displayController),
                 static_cast<unsigned>(BoardConfig::ACTIVE.displayControllerVariant),
                 static_cast<unsigned>(BoardConfig::ACTIVE.displayWidth),
                 static_cast<unsigned>(BoardConfig::ACTIVE.displayHeight), probe.valid, probe.verdict,
                 probe.promoted, probe.busyTimedOut, probe.ver[0], probe.ver[1], probe.ver[2], probe.ver[3], probe.ver[4],
                 probe.verBytesRead, probe.flg);
}

void settings(uint32_t font, uint32_t pointSize, uint32_t spacing, uint32_t orientation, uint32_t antialias,
              uint32_t inverted, uint32_t margin, uint32_t sleepScreen, uint32_t sdFontId) {
  if (!enabled()) return;
  // Slot 4 carries the page-clearing policy version (schema 2 stored the fixed
  // page count there).
  const uint32_t values[] = {font,     pointSize, spacing, orientation, AdaptiveRefreshPolicy::kVersion, antialias,
                             inverted, margin,    sleepScreen, sdFontId};
  Event event;
  event.type = EventType::ReaderSettings;
  event.startUs = event.endUs = nowUs();
  taskENTER_CRITICAL(&queueMux);
  if (recording.load(std::memory_order_relaxed) &&
      (!haveSettings || memcmp(values, previousSettings, sizeof(values)) != 0)) {
    const bool roomForPair = buffer.size() <= Buffer::CAPACITY - 2;
    memcpy(event.data, values, sizeof(event.data));
    buffer.push(event);
    event.type = EventType::DisplaySettings;
    memset(event.data, 0, sizeof(event.data));
    event.data[0] = sleepScreen;
    event.data[1] = sdFontId;
    buffer.push(event);
    // Retry the snapshot at the next render if overflow dropped either half.
    if (roomForPair) {
      memcpy(previousSettings, values, sizeof(values));
      haveSettings = true;
    }
  }
  taskEXIT_CRITICAL(&queueMux);
}

void record(const Event& event) {
  if (!enabled()) return;
  taskENTER_CRITICAL(&queueMux);
  if (recording.load(std::memory_order_relaxed)) buffer.push(event);
  taskEXIT_CRITICAL(&queueMux);
}

void point(EventType type, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  if (!enabled()) return;
  Event event;
  event.type = type;
  event.startUs = event.endUs = nowUs();
  event.data[0] = a;
  event.data[1] = b;
  event.data[2] = c;
  event.data[3] = d;
  record(event);
}

void span(EventType type, uint64_t begun, uint32_t a, uint32_t b, uint32_t c) {
  if (!begun || !enabled()) return;
  Event event;
  event.type = type;
  event.startUs = begun;
  event.endUs = nowUs();
  event.data[0] = a;
  event.data[1] = b;
  event.data[2] = c;
  record(event);
  if (type == EventType::Display && probeActive.load(std::memory_order_relaxed)) {
    taskENTER_CRITICAL(&probeMux);
    refreshProbe.onDisplayCall(static_cast<uint8_t>(a), b, event.startUs, event.endUs);
    taskEXIT_CRITICAL(&probeMux);
  }
}

namespace nav {
namespace {
std::atomic<bool> runActiveFlag{false};
std::atomic<bool> syncPermitFlag{false};
}  // namespace

void setRunActive(bool active) { runActiveFlag.store(active, std::memory_order_relaxed); }
bool runActive() { return runActiveFlag.load(std::memory_order_relaxed); }
void setSyncPermit(bool permitted) { syncPermitFlag.store(permitted, std::memory_order_relaxed); }
bool syncPermit() { return syncPermitFlag.load(std::memory_order_relaxed); }

void armProbe(uint64_t atUs) {
  taskENTER_CRITICAL(&probeMux);
  refreshProbe.arm(atUs);
  taskEXIT_CRITICAL(&probeMux);
  probeActive.store(true, std::memory_order_relaxed);
}

void disarmProbe() { probeActive.store(false, std::memory_order_relaxed); }

RefreshSnapshot probeSnapshot() {
  taskENTER_CRITICAL(&probeMux);
  const RefreshSnapshot snapshot = refreshProbe.snapshot();
  taskEXIT_CRITICAL(&probeMux);
  return snapshot;
}
}  // namespace nav

void intervalMs(EventType type, uint32_t startMs, uint32_t endMs, uint32_t code, uint32_t detail) {
  if (!enabled()) return;
  Event event;
  event.type = type;
  event.endUs = nowUs();
  const uint64_t duration = static_cast<uint64_t>(static_cast<uint32_t>(endMs - startMs)) * 1000;
  event.startUs = duration <= event.endUs ? event.endUs - duration : 0;
  event.data[0] = code;
  event.data[1] = startMs;
  event.data[2] = endMs;
  event.data[3] = detail;
  record(event);
}

void memory() {
  if (!enabled()) return;
  Event event;
  event.type = EventType::Memory;
  event.startUs = event.endUs = nowUs();
  const auto internal = HalMemory::getInternalHeap();
  const auto psram = HalMemory::getPsramHeap();
  event.data[0] = internal.freeBytes;
  event.data[1] = internal.totalBytes;
  event.data[2] = internal.minFreeBytes;
  event.data[3] = internal.largestBlockBytes;
  event.data[4] = psram.freeBytes;
  event.data[5] = psram.totalBytes;
  event.data[6] = psram.minFreeBytes;
  event.data[7] = psram.largestBlockBytes;
  record(event);
}

bool flushDue() {
  return enabled() && static_cast<uint32_t>(millis() - lastFlushMs.load(std::memory_order_relaxed)) >= 2000;
}

void flush() {
  if (!enabled()) return;
  IoLock lock;
  if (!enabled()) return;
  if (nowUs() - lastMemoryUs >= 10000000) {
    memory();
    lastMemoryUs = nowUs();
  }
  drainLocked(false, StopReason::Restart);
}

bool stop(StopReason reason) {
  if (!ioMutex) return false;
  IoLock lock;
  if (!recording.exchange(false, std::memory_order_relaxed)) return false;
  // Pair with producers' critical section: none can enqueue after this barrier.
  taskENTER_CRITICAL(&queueMux);
  taskEXIT_CRITICAL(&queueMux);
  return drainLocked(true, reason);
}

}  // namespace benchmark
