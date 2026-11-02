#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "ReadingStatsCore.h"

using namespace reading_stats;
#define CHECK(expression)                                           \
  do {                                                              \
    if (!(expression)) {                                            \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
      std::exit(1);                                                 \
    }                                                               \
  } while (0)

class MemoryFiles final : public Files {
 public:
  std::map<std::string, std::vector<uint8_t>> entries;
  bool fail = false;
  unsigned writes = 0;
  bool read(const char* path, Header& h, void* data, size_t size) override {
    auto it = entries.find(path);
    if (it == entries.end() || it->second.size() != sizeof(h) + size) return false;
    memcpy(&h, it->second.data(), sizeof(h));
    memcpy(data, it->second.data() + sizeof(h), size);
    return true;
  }
  bool commit(const char* path, const Header& h, const void* data, size_t size) override {
    if (fail) return false;
    auto& bytes = entries[path];
    bytes.resize(sizeof(h) + size);
    memcpy(bytes.data(), &h, sizeof(h));
    memcpy(bytes.data() + sizeof(h), data, size);
    ++writes;
    return true;
  }
};
int main() {
  const uint32_t day = civilDay(2026, 11, 1);
  char date[20];
  formatDay(day, date, sizeof(date));
  CHECK(std::string(date) == "2026-11-01");
  CHECK(civilDay(2024, 3, 1) - civilDay(2024, 2, 28) == 2);
  Accumulator a;
  a.begin(1000, day, 36000, 100);
  a.tick(61000, day, 36060, true);
  a.page(200);
  CHECK(a.book.totals.milliseconds == 60000);
  CHECK(a.book.totals.pages == 1 && a.book.totals.sessions == 1);
  CHECK(a.book.advanced == 100);
  a.page(150);
  CHECK(a.book.advanced == 100 && a.book.totals.pages == 2);
  CHECK(a.book.firstDay == day && a.book.lastDay == day);
  a.tick(661000, day, 36660, false);
  CHECK(a.book.totals.milliseconds == 360000);  // Only 5 min since last input.
  a.tick(761000, day, 36760, true);
  CHECK(a.book.totals.milliseconds == 360000);  // Input doesn't backfill idle time.
  a.tick(762000, day, 36761, false);
  CHECK(a.book.totals.milliseconds == 361000);
  a.pause(763000, day, 36762);
  a.tick(900000, day, 36900, true);
  CHECK(a.book.totals.milliseconds == 362000);
  a.resume(1000000, day, 37000);
  a.tick(1001000, day, 37001, false);
  CHECK(a.book.totals.milliseconds == 363000 && a.book.totals.sessions == 1);
  a.begin(1002000, day, 37002, 150);
  CHECK(a.book.totals.sessions == 2);  // Reopen/wake, not a menu return.

  Accumulator midnight;
  midnight.begin(0, day, 86390, 0);
  midnight.tick(20000, day + 1, 10, false);
  CHECK(midnight.recent(day, 1).milliseconds == 10000);
  CHECK(midnight.recent(day + 1, 1).milliseconds == 10000);
  CHECK(midnight.recent(day + 1, 7).milliseconds == 20000);
  CHECK(midnight.book.firstDay == day && midnight.book.lastDay == day + 1);
  CHECK(midnight.recent(day + 31, 30).milliseconds == 0);
  midnight.pause(20000, day + 1, 10);
  midnight.begin(30000, day + 31, 10, 0);
  midnight.tick(31000, day + 31, 11, false);
  CHECK(midnight.recent(day + 31, 30).milliseconds == 1000);

  Accumulator wrap;
  wrap.begin(0xfffffff0U, day, 0, 0);
  wrap.tick(984, day, 1, false);
  CHECK(wrap.book.totals.milliseconds == 1000);
  Accumulator unknown;
  unknown.begin(0, 0, 0, 0);
  unknown.tick(1000, 0, 0, false);
  CHECK(unknown.book.totals.milliseconds == 1000 && unknown.book.firstDay == 0);
  CHECK(unknown.recent(day, 30).milliseconds == 0);

  MemoryFiles files;
  uint32_t generation = 0;
  a.book.id = bookId("/books/test.epub");
  snprintf(a.book.title, sizeof(a.book.title), "Chinese and English");
  CHECK(save(files, "book", &a.book, sizeof(a.book), generation));
  CHECK(generation == 1);
  Book recovered;
  uint32_t loadedGeneration = 0;
  CHECK(load(files, "book", &recovered, sizeof(recovered), loadedGeneration));
  CHECK(memcmp(&recovered, &a.book, sizeof(recovered)) == 0 && loadedGeneration == 1);
  a.book.totals.pages = 99;
  CHECK(save(files, "book", &a.book, sizeof(a.book), generation));
  files.entries["book.0"].back() ^= 1;
  CHECK(load(files, "book", &recovered, sizeof(recovered), loadedGeneration));
  CHECK(recovered.totals.pages == 2 && loadedGeneration == 1);  // Previous complete generation.
  files.entries["book.1"].resize(7);
  CHECK(!load(files, "book", &recovered, sizeof(recovered), loadedGeneration));
  CHECK(recovered.id == 0 && recovered.totals.pages == 0 && loadedGeneration == 0);
  files.fail = true;
  CHECK(!save(files, "book", &a.book, sizeof(a.book), generation));
  CHECK(generation == 2);
  files.fail = false;
  a.updateTop();
  CHECK(a.summary.top[0].id == a.book.id);
  a.book.totals.milliseconds += 1000;
  a.updateTop();
  CHECK(a.summary.top[1].id == 0);  // Same book updates, never duplicates.
  CHECK(save(files, "daily", &a.summary, sizeof(a.summary), generation));
  Summary summary;
  CHECK(load(files, "daily", &summary, sizeof(summary), loadedGeneration));
  CHECK(memcmp(&summary, &a.summary, sizeof(summary)) == 0);

  // One hour with input each minute: 20 checkpoints, two logical file
  // commits each; no page events trigger a write. Lifecycle flushes are extra.
  Accumulator hour;
  hour.begin(0, day, 0, 0);
  unsigned checkpoints = 0;
  for (uint32_t now = 60000; now <= 3600000; now += 60000) {
    hour.tick(now, day, now / 1000, true);
    hour.page(now / 60000);
    if (now % FLUSH_MS == 0) ++checkpoints;
  }
  CHECK(hour.book.totals.milliseconds == 3600000 && hour.book.totals.pages == 60);
  CHECK(checkpoints == 20);
  printf(
      "PASS: accumulation, idle cutoff/resume, foreground pause, sessions, midnight, rolling window, clock unknown, "
      "millis wrap, progress, round-trip, corruption fallback, truncation reset, failed commit, top books, one-hour "
      "write budget\n");
  printf("sizeof(Book)=%zu sizeof(Summary)=%zu sizeof(Accumulator)=%zu; hourly commits=%u\n", sizeof(Book),
         sizeof(Summary), sizeof(Accumulator), checkpoints * 2);
}
