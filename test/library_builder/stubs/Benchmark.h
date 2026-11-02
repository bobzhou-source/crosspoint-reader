#pragma once

#include "../../../lib/Benchmark/BenchmarkBuffer.h"

// Library-builder host tests have no mounted diagnostic session. Exercise the
// real recorder's buffering/failure behavior in test/benchmark instead.
namespace benchmark {
inline void intervalMs(EventType, uint32_t, uint32_t, uint32_t = 0, uint32_t = 0) {}
}  // namespace benchmark
