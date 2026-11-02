#include <HalStorage.h>

#include <cstdio>
#include <cstdlib>

#include "ReadingStats.h"

uint32_t testMillis = 0;
#define CHECK(expression)                                           \
  do {                                                              \
    if (!(expression)) {                                            \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
      std::exit(1);                                                 \
    }                                                               \
  } while (0)
int main() {
  using namespace reading_stats;
  begin("/books/runtime.txt", "Runtime", 0);
  for (testMillis = 1000; testMillis <= 3600000; testMillis += 1000) {
    tick(testMillis % 60000 == 0);
    if (testMillis % 60000 == 0) {
      const auto writesBefore = testWriteCalls;
      page(testMillis / 60000);
      CHECK(testWriteCalls == writesBefore);
    }
  }
  CHECK(data().book.totals.milliseconds == 3600000);
  CHECK(data().book.totals.pages == 60);
  CHECK(testRenames == 40 && testSyncs == 40 && testWriteCalls == 80);
  printf(
      "PASS: production one-hour cadence: 20 checkpoints, %u record commits, %u HalFile::write calls, %u syncs; no "
      "per-page writes\n",
      testRenames, testWriteCalls, testSyncs);
  testMillis = 3600000;
  suspend();
  const auto savedTime = data().book.totals.milliseconds;
  const auto savedPages = data().book.totals.pages;
  const auto savedSessions = data().book.totals.sessions;
  testMillis += 600000;  // Menu is foreground; do not call the reader tick.
  tick(true);
  CHECK(data().book.totals.milliseconds == savedTime);
  CHECK(data().book.totals.sessions == savedSessions);
  testMillis += 1000;
  tick(false);
  CHECK(data().book.totals.milliseconds == savedTime + 1000);
  finish();
  // Opening a different book and returning forces the production file loader.
  begin("/books/other.xtc", "Other", 0);
  finish();
  begin("/books/runtime.txt", "Runtime", 60);
  CHECK(data().book.totals.milliseconds == savedTime + 1000);
  CHECK(data().book.totals.pages == savedPages);
  CHECK(data().book.totals.sessions == savedSessions + 1);
  const auto beforeIdle = data().book.totals.milliseconds;
  testMillis += 600000;
  tick(false);
  CHECK(data().book.totals.milliseconds == beforeIdle + IDLE_MS);
  const auto beforeFailure = testWriteCalls;
  testFailSync = true;
  page(61);
  CHECK(!flush());
  const auto afterFailure = testWriteCalls;
  CHECK(afterFailure > beforeFailure);
  for (unsigned i = 0; i < 100; ++i) {
    ++testMillis;
    tick(false);
  }
  CHECK(testWriteCalls == afterFailure);  // Failed writes do not retry each loop.
  testFailSync = false;
  CHECK(flush());
  printf(
      "PASS: foreground suspension, resume without a new session, saved reopen, idle cutoff, failed-save retry "
      "throttling\n");
}
