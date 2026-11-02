#pragma once

#include "ReadingStatsCore.h"

namespace reading_stats {
// Main-task only; UI takes a snapshot in onEnter before its render task runs.
Accumulator& data();
uint32_t today();
void begin(const char* path, const char* title, uint32_t progress);
void tick(bool input);
void page(uint32_t progress);
void suspend();
void finish();
bool flush();
}  // namespace reading_stats
