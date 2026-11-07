#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace reading_stats {
constexpr uint32_t IDLE_MS = 300000;
constexpr uint32_t FLUSH_MS = 180000;
constexpr size_t TOP_COUNT = 10;
struct Totals {
  uint64_t milliseconds = 0;
  uint32_t pages = 0;
  uint32_t sessions = 0;
};
struct Book {
  uint64_t id = 0;
  Totals totals;
  uint32_t firstDay = 0;
  uint32_t lastDay = 0;
  uint32_t advanced = 0;  // hundredths of a percent, forward movement only
  uint32_t position = 0;
  char title[96] = {};
};
struct Day {
  Totals totals;
  uint32_t date = 0;  // local civil day ordinal; zero means clock unavailable
  uint32_t reserved = 0;
};
struct Top {
  uint64_t id = 0;
  uint64_t milliseconds = 0;
  char title[96] = {};
};
struct Summary {
  Day days[30];
  Top top[TOP_COUNT];
};
struct Habit {
  uint32_t currentStreakDays = 0;
  uint32_t bestStreakDays = 0;
  uint32_t todayPagesRead = 0;
  uint32_t todayMinutesRead = 0;
};
uint32_t civilDay(int year, unsigned month, unsigned day);
void formatDay(uint32_t day, char* out, size_t size);
uint64_t bookId(const char* path);
uint32_t checksum(const void* data, size_t size);

// All calls are on the main task. Millis subtraction deliberately tolerates wrap.
class Accumulator {
 public:
  Book book;
  Summary summary;
  bool dirty = false;
  void begin(uint32_t now, uint32_t day, uint32_t secondOfDay, uint32_t progress);
  void tick(uint32_t now, uint32_t day, uint32_t secondOfDay, bool input);
  void pause(uint32_t now, uint32_t day, uint32_t secondOfDay);
  void resume(uint32_t now, uint32_t day, uint32_t secondOfDay);
  void page(uint32_t progress);
  bool active() const { return running; }
  Totals recent(uint32_t day, unsigned count) const;
  void updateTop();
  Habit habit(uint32_t today) const;

 private:
  bool running = false;
  uint32_t lastTick = 0;
  uint32_t lastInput = 0;
  uint32_t previousDay = 0;
  uint32_t previousSecond = 0;
  Day& daily(uint32_t day);
  void touchDate(uint32_t day);
};

// Native and firmware storage use the same validation and recovery algorithm.
// Each slot is independently checksummed; a failed commit leaves the other intact.
struct Header {
  uint32_t magic = 0x52535438;
  uint32_t version = 1;
  uint32_t bytes = 0;
  uint32_t generation = 0;
  uint32_t crc = 0;
};
static_assert(sizeof(Book) == 136 && sizeof(Summary) == 1840 && sizeof(Header) == 20,
              "Reading statistics version 1 layout changed");
class Files {
 public:
  virtual ~Files() = default;
  virtual bool read(const char* path, Header& header, void* data, size_t size) = 0;
  virtual bool commit(const char* path, const Header& header, const void* data, size_t size) = 0;
};
bool load(Files& files, const char* stem, void* data, size_t size, uint32_t& generation);
bool save(Files& files, const char* stem, const void* data, size_t size, uint32_t& generation);
}  // namespace reading_stats
