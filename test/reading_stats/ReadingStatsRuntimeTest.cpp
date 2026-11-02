#include <Benchmark.h>
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

  // --- stats_flush rows: written by the real flush path, only while a benchmark session is recording ---
  using benchmark::StatsFlushReason;
  using benchmark::StatsFlushResult;
  CHECK(testBenchmarkEvents.empty() && testBenchmarkStarts == 0);  // the whole run above had no session
  finish();
  CHECK(testBenchmarkEvents.empty());
  const auto checkRow = [](const benchmark::Event& row, StatsFlushReason reason, StatsFlushResult result,
                           uint32_t dirty) {
    CHECK(row.type == benchmark::EventType::StatsFlush);
    CHECK(row.endUs > row.startUs);  // the span is the call
    CHECK(row.data[0] == static_cast<uint32_t>(reason));
    CHECK(row.data[1] == static_cast<uint32_t>(result));
    CHECK(row.data[6] == dirty);
    CHECK(row.data[7] == 0);
  };
  testBenchmarkLive = true;
  testMillis += 1000;
  begin("/books/flush-rows.txt", "Flush rows", 0);
  const uint32_t openedAt = testMillis;
  testMillis = openedAt + 179999;
  tick(false);
  CHECK(testBenchmarkEvents.empty());  // not due yet: no flush, so no row
  testMillis = openedAt + 180000;
  const auto writesBeforePeriodic = testWriteCalls;
  tick(false);
  CHECK(testWriteCalls > writesBeforePeriodic);
  CHECK(testBenchmarkEvents.size() == 1);
  const auto periodic = testBenchmarkEvents[0];
  checkRow(periodic, StatsFlushReason::Periodic, StatsFlushResult::Saved, 1);
  CHECK(periodic.data[2] >= 1 && periodic.data[3] >= 1);  // both records were committed
  CHECK(periodic.data[4] == 180000 && periodic.data[5] == 0);
  // A checkpoint that fires late (the render lock deferred it) reports how late.
  testMillis += 500;
  tick(false);
  CHECK(testBenchmarkEvents.size() == 1);
  testMillis = openedAt + 180000 + 190000;
  tick(false);
  CHECK(testBenchmarkEvents.size() == 2);
  const auto late = testBenchmarkEvents[1];
  checkRow(late, StatsFlushReason::Periodic, StatsFlushResult::Saved, 1);
  CHECK(late.data[4] == 190000 && late.data[5] == 10000);
  CHECK(late.data[2] > periodic.data[2] && late.data[3] > periodic.data[3]);  // generations advance per save
  // Nothing dirty: a direct flush writes nothing and says so, with the generations unchanged.
  const auto writesBeforeClean = testWriteCalls;
  CHECK(flush());
  CHECK(testWriteCalls == writesBeforeClean);
  CHECK(testBenchmarkEvents.size() == 3);
  checkRow(testBenchmarkEvents[2], StatsFlushReason::Explicit, StatsFlushResult::Clean, 0);
  CHECK(testBenchmarkEvents[2].data[2] == late.data[2] && testBenchmarkEvents[2].data[3] == late.data[3]);
  // Covering the reader and leaving it are their own reasons.
  page(5);
  suspend();
  CHECK(testBenchmarkEvents.size() == 4);
  checkRow(testBenchmarkEvents[3], StatsFlushReason::Suspend, StatsFlushResult::Saved, 1);
  finish();
  CHECK(testBenchmarkEvents.size() == 5);
  checkRow(testBenchmarkEvents[4], StatsFlushReason::Exit, StatsFlushResult::Clean, 0);
  CHECK(flush());  // no book open: no flush reached a book, so no row
  CHECK(testBenchmarkEvents.size() == 5);
  // A failed save is recorded as failed and keeps the retry throttle: the next attempt is a full interval later.
  testMillis += 1000;
  begin("/books/flush-rows.txt", "Flush rows", 0);
  page(7);
  testFailSync = true;
  const uint32_t failedAt = testMillis;
  CHECK(!flush());
  CHECK(testBenchmarkEvents.size() == 6);
  checkRow(testBenchmarkEvents[5], StatsFlushReason::Explicit, StatsFlushResult::Failed, 1);
  const auto writesAfterFailure = testWriteCalls;
  for (unsigned i = 0; i < 100; ++i) {
    ++testMillis;
    tick(false);
  }
  CHECK(testWriteCalls == writesAfterFailure && testBenchmarkEvents.size() == 6);
  testFailSync = false;
  testMillis = failedAt + 179999;
  tick(false);
  CHECK(testBenchmarkEvents.size() == 6);
  testMillis = failedAt + 180000;
  tick(false);
  CHECK(testBenchmarkEvents.size() == 7);
  checkRow(testBenchmarkEvents[6], StatsFlushReason::Periodic, StatsFlushResult::Saved, 1);
  CHECK(testBenchmarkEvents[6].data[4] == 180000);
  // No session: the same saves happen and nothing is recorded or timed.
  testBenchmarkLive = false;
  const auto startsBefore = testBenchmarkStarts;
  page(9);
  const auto writesBeforeQuiet = testWriteCalls;
  CHECK(flush());
  CHECK(testWriteCalls > writesBeforeQuiet);
  CHECK(testBenchmarkEvents.size() == 7 && testBenchmarkStarts == startsBefore);
  printf("PASS: stats_flush rows carry reason, result, generations, interval and lateness; failed-save throttle kept\n");
  // --- suspend is a transition, not a per-loop flush: the covered reader calls it every ~11 ms ---
  testBenchmarkLive = true;
  testMillis += 1000;
  begin("/books/suspend-idempotent.txt", "Suspend idempotent", 0);
  testMillis += 1000;
  tick(false);  // active and credited, so the book is dirty
  const size_t rowsBefore = testBenchmarkEvents.size();
  suspend();
  CHECK(testBenchmarkEvents.size() == rowsBefore + 1);
  checkRow(testBenchmarkEvents[rowsBefore], StatsFlushReason::Suspend, StatsFlushResult::Saved, 1);
  const auto writesAfterSuspend = testWriteCalls;
  for (unsigned i = 0; i < 200; ++i) {  // a covered reader loops; nothing more to pause or save
    testMillis += 11;
    suspend();
  }
  CHECK(testBenchmarkEvents.size() == rowsBefore + 1 && testWriteCalls == writesAfterSuspend);
  // Resuming and covering again is a new transition with its own row.
  testMillis += 1000;
  tick(false);
  testMillis += 1000;
  tick(false);
  suspend();
  CHECK(testBenchmarkEvents.size() == rowsBefore + 2);
  checkRow(testBenchmarkEvents[rowsBefore + 1], StatsFlushReason::Suspend, StatsFlushResult::Saved, 1);
  suspend();
  CHECK(testBenchmarkEvents.size() == rowsBefore + 2);
  // A failed save leaves the book dirty: each suspend still retries it and reports Failed; the first success is recorded.
  testMillis += 1000;
  tick(false);
  testMillis += 1000;
  tick(false);
  testFailSync = true;
  suspend();
  CHECK(testBenchmarkEvents.size() == rowsBefore + 3);
  checkRow(testBenchmarkEvents[rowsBefore + 2], StatsFlushReason::Suspend, StatsFlushResult::Failed, 1);
  const auto writesBeforeRetry = testWriteCalls;
  testMillis += 11;
  suspend();
  CHECK(testBenchmarkEvents.size() == rowsBefore + 4 && testWriteCalls > writesBeforeRetry);
  checkRow(testBenchmarkEvents[rowsBefore + 3], StatsFlushReason::Suspend, StatsFlushResult::Failed, 1);
  testFailSync = false;
  testMillis += 11;
  suspend();
  CHECK(testBenchmarkEvents.size() == rowsBefore + 5);
  checkRow(testBenchmarkEvents[rowsBefore + 4], StatsFlushReason::Suspend, StatsFlushResult::Saved, 1);
  testMillis += 11;
  suspend();
  CHECK(testBenchmarkEvents.size() == rowsBefore + 5);
  // A long covered interval no longer refreshes lastFlush (the covered loop's clean suspends are skipped). The first
  // zero-credit resume tick is then a due periodic checkpoint, but the book is clean, so it writes nothing: one Clean
  // row, late by the covered time. Later credited time is saved on the normal cadence from that checkpoint.
  testMillis += 400000;
  const size_t rowsAtCover = testBenchmarkEvents.size();
  const auto writesAtCover = testWriteCalls;
  tick(false);
  CHECK(testWriteCalls == writesAtCover);
  CHECK(testBenchmarkEvents.size() == rowsAtCover + 1);
  const auto resumeRow = testBenchmarkEvents[rowsAtCover];
  CHECK(resumeRow.type == benchmark::EventType::StatsFlush);
  CHECK(resumeRow.data[0] == static_cast<uint32_t>(StatsFlushReason::Periodic));
  CHECK(resumeRow.data[1] == static_cast<uint32_t>(StatsFlushResult::Clean));
  CHECK(resumeRow.data[6] == 0);
  testMillis += 1000;
  tick(false);  // credited again: dirty, but the checkpoint was just taken
  CHECK(testWriteCalls == writesAtCover && testBenchmarkEvents.size() == rowsAtCover + 1);
  testMillis += 179000;
  tick(false);
  CHECK(testWriteCalls > writesAtCover && testBenchmarkEvents.size() == rowsAtCover + 2);
  checkRow(testBenchmarkEvents[rowsAtCover + 1], StatsFlushReason::Periodic, StatsFlushResult::Saved, 1);
  CHECK(testBenchmarkEvents[rowsAtCover + 1].data[4] == 180000);
  suspend();  // still active after the resume: one Suspend/Clean row for the transition, none for the repeat
  CHECK(testBenchmarkEvents.size() == rowsAtCover + 3);
  checkRow(testBenchmarkEvents[rowsAtCover + 2], StatsFlushReason::Suspend, StatsFlushResult::Clean, 0);
  suspend();
  CHECK(testBenchmarkEvents.size() == rowsAtCover + 3);
  // Leaving always flushes (one Exit row, even when clean); suspend with no book open does nothing.
  finish();
  CHECK(testBenchmarkEvents.size() == rowsAtCover + 4);
  checkRow(testBenchmarkEvents[rowsAtCover + 3], StatsFlushReason::Exit, StatsFlushResult::Clean, 0);
  suspend();
  CHECK(testBenchmarkEvents.size() == rowsAtCover + 4);
  testBenchmarkLive = false;
  printf("PASS: repeated suspend while covered is idempotent (one row per transition); dirty and failed saves still retry\n");
}
