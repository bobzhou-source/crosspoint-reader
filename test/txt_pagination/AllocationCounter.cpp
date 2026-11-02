#include "AllocationCounter.h"

#include <cstdlib>
#include <new>

namespace {
bool counting = false;
size_t allocations = 0;
}  // namespace

namespace alloc_counter {
void start() {
  allocations = 0;
  counting = true;
}

size_t stop() {
  counting = false;
  return allocations;
}
}  // namespace alloc_counter

// Array and nothrow forms forward to this one.
void* operator new(std::size_t size) {
  if (counting) ++allocations;
  if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
  throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }

void operator delete(void* p, std::size_t) noexcept { std::free(p); }
