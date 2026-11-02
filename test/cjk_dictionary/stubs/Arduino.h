#pragma once
#include <cstdint>
inline unsigned long millis() { return 0; }
struct HostEsp {
  uint32_t heap = 1024 * 1024;
  uint32_t getMaxAllocHeap() { return heap; }
};
inline HostEsp ESP;
