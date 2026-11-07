#pragma once

#include <cstddef>

// Compact "12-day streak · 34 pages today" line for the home header and the
// sleep screen. Returns nullptr when the clock is unset or the reader has no
// habit to show yet, so callers can draw nothing instead of a zero. Main-task
// only: it reads the reading-stats accumulator.
const char* habitBadge(char* buffer, size_t size);
