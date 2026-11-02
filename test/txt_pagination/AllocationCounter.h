#pragma once

#include <cstddef>

// Counts global operator new calls between start() and stop() (replaced in AllocationCounter.cpp).
namespace alloc_counter {
void start();
size_t stop();
}  // namespace alloc_counter
