#include "ReaderSyncProtocol.h"

#include <Memory.h>
#include <cstring>
#include <cctype>

namespace reader_sync {
namespace {
bool hexKey(const char* key) {
  if (!key || strlen(key) != 64) return false;
  for (size_t i = 0; i < 64; ++i) if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f'))) return false;
  return true;
}
bool originValid(const char* origin) {
  // A root HTTPS origin only: no credentials, path, port, query, fragment or redirects.
  if (!origin || strncmp(origin, "https://", 8) != 0) return false;
  const size_t n = strlen(origin);
  if (n < 12 || n > 253 || origin[8] == '.' || origin[n - 1] == '.') return false;
  bool dot = false;
  for (size_t i = 8; i < n; ++i) {
    const unsigned char c = origin[i];
    if (c == '.') dot = true;
    else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return dot;
}
bool recover(const char* path) {
  const std::string backup = std::string(path) + ".bak";
  return Storage.exists(path) || !Storage.exists(backup.c_str()) || Storage.rename(backup.c_str(), path);
}
// The private marker is exactly the ".crosspoint" directory name. Other dot
// names keep their existing hidden-file policy in the web/WebDAV layers and
// are not part of this predicate.
bool privateComponent(const char* data, const size_t length) {
  constexpr char kName[] = ".crosspoint";
  if (length != sizeof(kName) - 1) return false;
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = static_cast<unsigned char>(data[i]);
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
    if (c != static_cast<unsigned char>(kName[i])) return false;
  }
  return true;
}
}  // namespace

bool privateWebPath(const char* path) {
  // HTTP/WebDAV decode URL arguments before calling this helper. Each existing
  // FAT component is identified by opening it and matching that handle's
  // directory-entry index against the entries read from its actual parent.
  // Only the enumerated entry carries the long name: SdFat leaves the LFN
  // ordinal at zero when a path opens through an 8.3 alias, so getName() on
  // the alias-opened handle returns the short name and must never be trusted.
  // New components are checked lexically, without banning ordinary '~' or '%'
  // filenames.
  if (!path || strlen(path) > 4096) return true;
  std::string resolved;
  bool directoryKnown = true;
  const char* p = path;
  while (*p) {
    while (*p == '/' || *p == '\\') ++p;
    const char* start = p;
    while (*p && *p != '/' && *p != '\\') {
      if (static_cast<unsigned char>(*p) < 0x20) return true;
      ++p;
    }
    const size_t n = p - start;
    if (!n) continue;
    // The private marker is matched case-insensitively, while ordinary '~'/'%'
    // names remain allowed. FAT trims dots/spaces, so those spellings are
    // rejected before opening any candidate.
    if (privateComponent(start, n) || start[n - 1] == ' ' || start[n - 1] == '.') return true;
    // An absent ancestor proves the rest of the path cannot resolve through
    // it; those components can only be checked lexically.
    if (!directoryKnown) continue;
    auto directory = Storage.open(resolved.empty() ? "/" : resolved.c_str());
    // A component that cannot be opened may still be a private alias, so fail
    // closed. An opened non-directory cannot be the private marker (which is a
    // directory), so the rest of the path can only be checked lexically.
    if (!directory) return true;
    if (!directory.isDirectory()) {
      directoryKnown = false;
      continue;
    }
    std::string candidate = resolved;
    candidate += '/';
    candidate.append(start, n);
    auto opened = Storage.open(candidate.c_str());
    if (!opened) {
      directoryKnown = false;
      continue;
    }
    const uint32_t identity = opened.dirIndex();
    opened.close();
    directory.rewindDirectory();
    unsigned matches = 0;
    bool anyPrivate = false;
    std::string canonical;
    bool canonicalDirectory = false;
    for (auto entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
      if (entry.dirIndex() != identity) continue;
      char name[256];
      const size_t length = entry.getName(name, sizeof(name));
      if (!length) return true;
      ++matches;
      canonical.assign(name, length);
      canonicalDirectory = entry.isDirectory();
      anyPrivate = anyPrivate || privateComponent(name, length);
    }
    if (anyPrivate) return true;
    // A missing or ambiguous identity cannot be resolved safely.
    if (matches != 1 || resolved.size() + canonical.size() + 1 > 4096) return true;
    resolved += '/';
    resolved += canonical;
    directoryKnown = canonicalDirectory;
  }
  return false;
}

bool atomicWrite(const char* path, const uint8_t* data, const size_t size) {
  if (!recover(path)) return false;
  const std::string temporary = std::string(path) + ".tmp";
  const std::string backup = std::string(path) + ".bak";
  auto file = Storage.open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return false;
  const bool written = file.write(data, size) == size && file.sync();
  const bool closed = file.close();
  if (!written || !closed) { Storage.remove(temporary.c_str()); return false; }
  // Readback catches short/card-full writes before replacing the previous state.
  auto check = Storage.open(temporary.c_str());
  bool matches = check && check.size() == size;
  for (size_t i = 0; matches && i < size; ++i) matches = check.read() == data[i];
  check.close();
  if (!matches) { Storage.remove(temporary.c_str()); return false; }
  const bool replacing = Storage.exists(path);
  if (replacing && ((Storage.exists(backup.c_str()) && !Storage.remove(backup.c_str())) ||
                    !Storage.rename(path, backup.c_str()))) return false;
  if (!Storage.rename(temporary.c_str(), path)) {
    if (replacing) Storage.rename(backup.c_str(), path);
    return false;
  }
  if (replacing) Storage.remove(backup.c_str());
  return true;
}

bool loadConfig(Config& config) {
  config = Config{};
  if (!recover(CONFIG_PATH)) return false;
  auto file = Storage.open(CONFIG_PATH);
  if (!file || file.size() == 0 || file.size() > 2048) return false;
  auto raw = makeUniqueNoThrow<char[]>(2049);
  if (!raw) return false;
  const size_t size = file.size();
  if (file.read(raw.get(), size) != static_cast<int>(size)) return false;
  raw[size] = '\0';
  JsonDocument doc;
  if (deserializeJson(doc, raw.get(), size) || !doc.is<JsonObject>()) return false;
  // Legacy files carry exactly the six original fields; an explicit "schedule"
  // string makes seven. Anything else, including a null schedule, is rejected.
  const bool hasSchedule = !doc["schedule"].isNull();
  if (doc.size() != (hasSchedule ? 7U : 6U) ||
      !doc["version"].is<unsigned>() || doc["version"] != 1 || !doc["enabled"].is<bool>() ||
      !doc["interval_seconds"].is<uint32_t>()) return false;
  const char* origin = doc["origin"].as<const char*>();
  const char* key = doc["device_key"].as<const char*>();
  const char* id = doc["device_id"].as<const char*>();
  const uint32_t interval = doc["interval_seconds"].as<uint32_t>();
  if (!originValid(origin) || !hexKey(key) || !id || !*id || strlen(id) > 128 || interval < 60 || interval > 86400) return false;
  if (doc["origin"].as<JsonString>().size() != strlen(origin) ||
      doc["device_key"].as<JsonString>().size() != strlen(key) ||
      doc["device_id"].as<JsonString>().size() != strlen(id)) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(id); *p; ++p) if (*p < 0x20 || *p == 0x7f) return false;
  Schedule schedule = Schedule::Events;
  if (hasSchedule) {
    const char* text = doc["schedule"].is<const char*>() ? doc["schedule"].as<const char*>() : nullptr;
    if (!parseSchedule(text, schedule) || doc["schedule"].as<JsonString>().size() != strlen(text)) return false;
  }
  config.origin = origin;
  config.key = key;
  config.deviceId = id;
  config.enabled = doc["enabled"].as<bool>();
  config.intervalSeconds = interval;
  config.schedule = schedule;
  config.scheduleExplicit = hasSchedule;
  memset(raw.get(), 0, size);
  return true;
}

bool saveConfig(const Config& config) {
  JsonDocument doc;
  doc["version"] = 1;
  doc["origin"] = config.origin;
  doc["device_key"] = config.key;
  doc["device_id"] = config.deviceId;
  doc["enabled"] = config.enabled;
  doc["interval_seconds"] = config.intervalSeconds;
  if (config.scheduleExplicit) doc["schedule"] = scheduleName(config.schedule);
  std::string raw;
  serializeJson(doc, raw);
  const bool ok = !doc.overflowed() && atomicWrite(CONFIG_PATH, reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
  std::fill(raw.begin(), raw.end(), '\0');
  return ok;
}
}  // namespace reader_sync
