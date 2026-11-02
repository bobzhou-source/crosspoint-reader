#include "ReaderSyncProtocol.h"
#include <openssl/rand.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>
#include <ctime>

namespace {
int64_t clockNow() { return time(nullptr); }
void randomBytes(uint8_t* out, size_t n) { if (RAND_bytes(out, static_cast<int>(n)) != 1) std::abort(); }
void invalidateCache(const std::string& path) {
  // Cache invalidation is a platform hook, not part of the transfer algorithm.
  std::filesystem::remove_all(Storage.root / ".crosspoint" / ("epub_" + std::to_string(std::hash<std::string>{}(path))));
}
bool cancel(void*) {
  if (!getenv("READER_TEST_CANCEL")) return false;
  const auto folder = Storage.resolve(reader_sync::PRIVATE_ROOT);
  if (!std::filesystem::exists(folder)) return false;
  for (const auto& entry : std::filesystem::directory_iterator(folder)) {
    if (entry.path().extension() == ".part" && entry.file_size() >= 16384) return true;
  }
  return false;
}
}
int main(int argc, char** argv) {
  if (argc != 5) return 2;
  Storage.root = argv[1];
  // A POSIX symlink cannot model a FAT alternate name: SdFat resolves an 8.3
  // alias to the same directory entry, while a symlink is a second entry whose
  // canonicalized name used to make the old guard look correct. Real alias and
  // case-alias coverage lives in test/reader_sync/run_fat_privacy.py, which
  // runs the pinned SdFat against an isolated FAT image.
  reader_sync::Config config;
  if (!reader_sync::loadConfig(config)) { std::cout << "{\"invalid_config\":true}\n"; return 0; }
  const std::string secret = config.key;
  config.enabled = !config.enabled;
  if (!reader_sync::saveConfig(config)) return 3;
  reader_sync::Config reloaded;
  if (!reader_sync::loadConfig(reloaded) || reloaded.key != secret || reloaded.enabled != config.enabled) return 4;
  // The schedule and whether it was written explicitly must round-trip: a legacy file stays legacy.
  if (reloaded.schedule != config.schedule || reloaded.scheduleExplicit != config.scheduleExplicit ||
      reloaded.intervalSeconds != config.intervalSeconds) return 9;
  config.enabled = !config.enabled;
  if (!reader_sync::saveConfig(config)) return 5;
  for (const char* path : {"/.crosspoint/reader-sync.json", "/.CROSSPOINT/reader-sync/status.json", "/x/../.crosspoint/reader-sync", "/x\\.crosspoint\\reader-sync", "/.crosspoint/reader-sync.json.bak"}) {
    if (!reader_sync::privateWebPath(path)) return 6;
  }
  std::ofstream(Storage.root / "notes~draft.json") << "public";
  std::ofstream(Storage.root / "50% complete.epub") << "public";
  std::ofstream(Storage.root / "a-very-long-public-name.txt") << "public";
  for (const char* path : {"/books/Inbox/a.epub", "/books/Inbox/notes~draft.json", "/books/Inbox/50% complete.epub",
                           "/.crosspoint-public.txt", "/notes~draft.json", "/50% complete.epub",
                           "/a-very-long-public-name.txt"}) {
    if (reader_sync::privateWebPath(path)) return 7;
  }
  for (const char* path : {"/notes~draft.json", "/50% complete.epub", "/a-very-long-public-name.txt"}) {
    auto file = Storage.open(path);
    char name[64];
    if (!file || !file.getName(name, sizeof(name)) || strcmp(name, path + 1) != 0) return 8;
  }
  std::ifstream trustFile(argv[3]);
  const std::string trust{std::istreambuf_iterator<char>(trustFile), {}};
  config.origin = argv[2];  // Test-only local HTTPS endpoint; production config rejects ports.
  const unsigned rounds = std::stoul(argv[4]);
  std::string after = getenv("READER_TEST_AFTER") ? getenv("READER_TEST_AFTER") : "";
  for (unsigned round = 0; round < rounds; ++round) {
    reader_sync::Protocol protocol(config, cancel, nullptr, clockNow, randomBytes, invalidateCache);
    protocol.setTestTrust(trust.c_str());
    const auto result = protocol.run(after);
    JsonDocument output;
    output["phase"] = static_cast<unsigned>(result.phase);
    output["books"] = result.books;
    output["notes"] = result.notes;
    output["rejected"] = result.rejectedNotes;
    output["more"] = result.more;
    output["retry"] = result.retrySeconds;
    output["after"] = result.after;
    output["schedule"] = reader_sync::scheduleName(config.schedule);
    output["explicit"] = config.scheduleExplicit;
    output["interval"] = config.intervalSeconds;
    output["rx"] = result.bytesReceived;
    output["tx"] = result.bytesSent;
    std::string json;
    serializeJson(output, json);
    std::cout << json << '\n';
    after = result.after;
  }
}
