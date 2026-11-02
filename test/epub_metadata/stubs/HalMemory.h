#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>

class HalMemory {
 public:
  struct HeapStats { size_t freeBytes, totalBytes, minFreeBytes, largestBlockBytes; };
  struct PsramDeleter { void operator()(uint8_t* p) const { std::free(p); } };
  using PsramBuffer = std::unique_ptr<uint8_t[], PsramDeleter>;
  static inline bool present = true;
  static inline bool fail = false;
  static inline size_t allocations = 0;
  static HeapStats getPsramHeap() { return {0, present ? 8388608u : 0u, 0, 0}; }
  static PsramBuffer allocatePsram(size_t bytes) {
    ++allocations;
    return PsramBuffer(fail ? nullptr : static_cast<uint8_t*>(std::malloc(bytes)));
  }
};
