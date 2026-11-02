#pragma once
// Host stand-in for the device recorder, enough for ReadingStats.cpp: the real Benchmark.h pulls in the whole nav run.
// Events are built by the real BenchmarkBuffer encoder and kept in a vector so a test can read what the flush path wrote.
#include <cstdint>
#include <vector>

#include "BenchmarkBuffer.h"

inline bool testBenchmarkLive = false;  // a session is recording
inline uint64_t testBenchmarkUs = 0;
inline unsigned testBenchmarkStarts = 0;  // start() calls that returned a nonzero clock read
inline std::vector<benchmark::Event> testBenchmarkEvents;

namespace benchmark {
// Each clock read advances time, so a recorded span is never empty.
inline uint64_t nowUs() { return testBenchmarkUs += 250; }
// Zero, with no clock read, while no session is recording (the real one's contract).
inline uint64_t start() {
  if (!testBenchmarkLive) return 0;
  ++testBenchmarkStarts;
  return nowUs();
}
inline void record(const Event& event) {
  if (testBenchmarkLive) testBenchmarkEvents.push_back(event);
}
}  // namespace benchmark
