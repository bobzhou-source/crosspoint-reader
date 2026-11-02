#include "ReadingStats.h"

#include <Arduino.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

namespace reading_stats {
namespace {
constexpr const char* DIRECTORY = "/.crosspoint/reading-stats";
constexpr const char* SUMMARY = "/.crosspoint/reading-stats/daily";
Accumulator accumulator;
uint32_t bookGeneration = 0;
uint32_t summaryGeneration = 0;
uint32_t lastFlush = 0;
bool loaded = false;
bool opened = false;
char bookStem[96] = {};
class SdFiles final : public Files {
 public:
  bool read(const char* path, Header& h, void* data, size_t size) override {
    HalFile file;
    return Storage.openFileForRead("STATS", path, file) && file.size() == sizeof(h) + size &&
           file.read(&h, sizeof(h)) == static_cast<int>(sizeof(h)) && file.read(data, size) == static_cast<int>(size);
  }
  bool commit(const char* path, const Header& h, const void* data, size_t size) override {
    char temp[136];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    HalFile file;
    if (!Storage.openFileForWrite("STATS", temp, file) || file.write(&h, sizeof(h)) != sizeof(h) ||
        file.write(data, size) != size || !file.sync())
      return false;
    if (!file.close()) return false;
    if (Storage.exists(path) && !Storage.remove(path)) return false;
    return Storage.rename(temp, path);
  }
} files;
void clock(uint32_t& day, uint32_t& second) {
  tm local{};
  day = second = 0;
  if (!halClock.localTime(local) || local.tm_year < 120 || local.tm_year > 199) return;
  day = civilDay(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
  second = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
}
}  // namespace
Accumulator& data() {
  if (!loaded) {
    load(files, SUMMARY, &accumulator.summary, sizeof(accumulator.summary), summaryGeneration);
    loaded = true;
  }
  return accumulator;
}
uint32_t today() {
  uint32_t day, second;
  clock(day, second);
  return day;
}
void begin(const char* path, const char* title, uint32_t progress) {
  data();
  if (opened) finish();
  const uint64_t id = bookId(path);
  snprintf(bookStem, sizeof(bookStem), "%s/%016llx", DIRECTORY, static_cast<unsigned long long>(id));
  load(files, bookStem, &accumulator.book, sizeof(accumulator.book), bookGeneration);
  if (accumulator.book.id != id) accumulator.book = {};
  accumulator.book.id = id;
  snprintf(accumulator.book.title, sizeof(accumulator.book.title), "%s", title);
  // Never retain a truncated UTF-8 tail in the title shown in top books.
  size_t n = strlen(accumulator.book.title);
  if (n == sizeof(accumulator.book.title) - 1) {
    while (n && (static_cast<unsigned char>(accumulator.book.title[n - 1]) & 0xc0) == 0x80) --n;
    if (n && static_cast<unsigned char>(accumulator.book.title[n - 1]) >= 0xc0) --n;
    accumulator.book.title[n] = 0;
  }
  uint32_t day, second;
  clock(day, second);
  lastFlush = millis();
  accumulator.begin(lastFlush, day, second, progress);
  opened = true;
}
void tick(bool input) {
  if (!opened) return;
  uint32_t day, second;
  clock(day, second);
  const uint32_t now = millis();
  if (!accumulator.active()) accumulator.resume(now, day, second);
  accumulator.tick(now, day, second, input);
  if (now - lastFlush >= FLUSH_MS) flush();
}
void page(uint32_t progress) {
  if (opened) accumulator.page(progress);
}
bool flush() {
  lastFlush = millis();  // Failure retries are throttled too.
  if (!opened || !accumulator.dirty) return true;
  accumulator.updateTop();
  if (!Storage.ensureDirectoryExists(DIRECTORY) ||
      !save(files, bookStem, &accumulator.book, sizeof(accumulator.book), bookGeneration) ||
      !save(files, SUMMARY, &accumulator.summary, sizeof(accumulator.summary), summaryGeneration)) {
    LOG_ERR("STATS", "Failed to save reading statistics");
    return false;
  }
  accumulator.dirty = false;
  return true;
}
void suspend() {
  if (!opened) return;
  uint32_t day, second;
  clock(day, second);
  accumulator.pause(millis(), day, second);
  flush();
}
void finish() {
  suspend();
  opened = false;
}
}  // namespace reading_stats
