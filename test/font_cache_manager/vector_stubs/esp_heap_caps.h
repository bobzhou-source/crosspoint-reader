#pragma once

// Host seam for TtfEpdFont's heap-funded cache growth gate (canGrow). The real
// ESP-IDF queries report PSRAM / internal DRAM headroom; the vector tests lower
// these to simulate a heap that cannot fund further cache growth.

#include <cstddef>
#include <cstdint>

#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_8BIT (1u << 2)

struct HostHeapState {
  static constexpr size_t kPlenty = 64u * 1024u * 1024u;
  size_t spiramLargest = kPlenty;
  size_t internalLargest = kPlenty;
  size_t internalFree = kPlenty;
};
inline HostHeapState g_hostHeap;

inline size_t heap_caps_get_largest_free_block(const uint32_t caps) {
  return (caps & MALLOC_CAP_SPIRAM) ? g_hostHeap.spiramLargest : g_hostHeap.internalLargest;
}

inline size_t heap_caps_get_free_size(const uint32_t) { return g_hostHeap.internalFree; }
