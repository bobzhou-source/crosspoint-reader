#pragma once

#include <HalStorage.h>
#include <SecureHttpClient.h>
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "ReaderSyncSchedule.h"

namespace reader_sync {
constexpr char CONFIG_PATH[] = "/.crosspoint/reader-sync.json";
constexpr char PRIVATE_ROOT[] = "/.crosspoint/reader-sync";
constexpr size_t MAX_BOOK_BYTES = 8 * 1024 * 1024;

struct Config {
  std::string origin;
  std::string key;
  std::string deviceId;
  uint32_t intervalSeconds = 900;
  bool enabled = false;
  // Absent from legacy six-field files, which keep loading and mean Events.
  Schedule schedule = Schedule::Events;
  // True only when the file carried a "schedule" key. saveConfig writes the key
  // only then, so a legacy file stays loadable by older firmware until the user
  // actually chooses a schedule.
  bool scheduleExplicit = false;
};

// Error values, never response bodies or credential-bearing URLs, cross the UI boundary.
enum class Phase : uint8_t {
  Unconfigured, Disabled, Waiting, Connecting, Syncing, Success, Offline,
  InvalidConfig, ClockError, MemoryError, StorageError, ProtocolError,
  AuthError, RateLimited, NeedsRedelivery, AnnotationError, Cancelled
};
struct Result {
  Phase phase = Phase::Success;
  uint32_t books = 0;
  uint32_t notes = 0;
  uint32_t rejectedNotes = 0;
  uint32_t retrySeconds = 0;
  bool more = false;
  bool parkUntilWake = false;
  std::string after;
  // Application payload bytes (request and response bodies), excluding HTTP
  // headers and TLS framing.
  uint32_t bytesSent = 0;
  uint32_t bytesReceived = 0;
};

bool loadConfig(Config& config);
bool saveConfig(const Config& config);
bool privateWebPath(const char* path);
bool atomicWrite(const char* path, const uint8_t* data, size_t size);

class Protocol {
 public:
  using Cancel = bool (*)(void*);
  using Clock = int64_t (*)();
  using Random = void (*)(uint8_t*, size_t);
  Protocol(const Config& config, Cancel cancel, void* context, Clock clock, Random random,
           void (*invalidateCache)(const std::string&));
  Result run(const std::string& after);
#ifdef READER_SYNC_HOST_TEST
  void setTestTrust(const char* pem) { http.setCACert(pem); }
#endif

 private:
  bool cancelled() const;
  bool prepare(const char* method, const std::string& route, const std::string& body);
  int request(const char* method, const std::string& route, const std::string& body,
              const freeink::SecureHttpClient::DataCallback& sink);
  bool responseOk(int code, int alternate = 200);
  bool readJson(const std::string& route, JsonDocument& doc, bool list);
  bool install(JsonVariantConst item);
  bool hashFile(const std::string& path, size_t size, const char* expected);
  bool uploadNotes();
  bool uploadNote(const std::string& path);
  const Config& config;
  Cancel cancel;
  void* context;
  Clock clock;
  Random random;
  void (*invalidateCache)(const std::string&);
  freeink::SecureHttpClient http;
  Result result;
  unsigned noteAttempts = 0;
  std::unique_ptr<uint8_t[]> buffer;
};
}  // namespace reader_sync
