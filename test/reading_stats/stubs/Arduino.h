#pragma once
#include <cstdint>
extern uint32_t testMillis;
inline unsigned long millis() { return testMillis; }
