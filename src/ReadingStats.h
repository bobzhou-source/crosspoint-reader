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
// Forgets the in-RAM statistics so the next use reloads them from the card. Call only with no book open.
void reload();
bool flush();
}  // namespace reading_stats
