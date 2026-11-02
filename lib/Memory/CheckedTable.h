#pragma once

#include <HalMemory.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>

// Count-scaled POD metadata: contiguous PSRAM on equipped boards; small internal
// blocks on boards without PSRAM. A PSRAM allocation failure never spills inward.
// All storage belongs to the table and every growth operation reports failure.
template <typename T>
class CheckedTable {
  static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
  static constexpr size_t BLOCK_ITEMS = 512 / sizeof(T) ? 512 / sizeof(T) : 1;
  const bool external = HalMemory::getPsramHeap().totalBytes != 0;
  HalMemory::PsramBuffer buffer;
  std::unique_ptr<T*[]> blocks;
  size_t blockCount = 0;
  size_t count = 0;
  size_t capacity = 0;

 public:
  CheckedTable() = default;
  CheckedTable(const CheckedTable&) = delete;
  CheckedTable& operator=(const CheckedTable&) = delete;
  ~CheckedTable() { clear(); }

  class Iterator {
    CheckedTable* owner;
    size_t index;

   public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = T*;
    using reference = T&;
    Iterator(CheckedTable* owner, size_t index) : owner(owner), index(index) {}
    reference operator*() const { return (*owner)[index]; }
    pointer operator->() const { return &**this; }
    reference operator[](difference_type n) const { return *(*this + n); }
    Iterator& operator++() {
      ++index;
      return *this;
    }
    Iterator operator++(int) {
      auto old = *this;
      ++*this;
      return old;
    }
    Iterator& operator--() {
      --index;
      return *this;
    }
    Iterator operator--(int) {
      auto old = *this;
      --*this;
      return old;
    }
    Iterator& operator+=(difference_type n) {
      index += n;
      return *this;
    }
    Iterator& operator-=(difference_type n) {
      index -= n;
      return *this;
    }
    Iterator operator+(difference_type n) const {
      auto result = *this;
      return result += n;
    }
    Iterator operator-(difference_type n) const {
      auto result = *this;
      return result -= n;
    }
    difference_type operator-(const Iterator& other) const {
      return static_cast<difference_type>(index) - static_cast<difference_type>(other.index);
    }
    friend Iterator operator+(difference_type n, Iterator it) { return it += n; }
    bool operator==(const Iterator& other) const { return index == other.index; }
    bool operator!=(const Iterator& other) const { return !(*this == other); }
    bool operator<(const Iterator& other) const { return index < other.index; }
    bool operator>(const Iterator& other) const { return other < *this; }
    bool operator<=(const Iterator& other) const { return !(other < *this); }
    bool operator>=(const Iterator& other) const { return !(*this < other); }
  };

  size_t size() const { return count; }
  bool empty() const { return count == 0; }
  T& operator[](size_t i) {
    return external ? reinterpret_cast<T*>(buffer.get())[i] : blocks[i / BLOCK_ITEMS][i % BLOCK_ITEMS];
  }
  const T& operator[](size_t i) const {
    return external ? reinterpret_cast<const T*>(buffer.get())[i] : blocks[i / BLOCK_ITEMS][i % BLOCK_ITEMS];
  }
  Iterator begin() { return Iterator(this, 0); }
  Iterator end() { return Iterator(this, count); }

  bool reserve(size_t n) {
    if (n <= capacity) return true;
    if (n > std::numeric_limits<size_t>::max() / sizeof(T)) return false;
    if (external) {
      auto next = HalMemory::allocatePsram(n * sizeof(T));
      if (!next) return false;
      if (count) std::memcpy(next.get(), buffer.get(), count * sizeof(T));
      buffer = std::move(next);
      capacity = n;
      return true;
    }
    const size_t needed = (n - 1) / BLOCK_ITEMS + 1;
    auto next = std::unique_ptr<T*[]>(new (std::nothrow) T*[needed]{});
    if (!next) return false;
    for (size_t i = 0; i < blockCount; ++i) next[i] = blocks[i];
    for (size_t i = blockCount; i < needed; ++i) {
      next[i] = new (std::nothrow) T[BLOCK_ITEMS];
      if (!next[i]) {
        for (size_t j = blockCount; j < i; ++j) delete[] next[j];
        return false;
      }
    }
    blocks = std::move(next);
    blockCount = needed;
    capacity = needed * BLOCK_ITEMS;
    return true;
  }

  bool resize(size_t n, T value = {}) {
    if (!reserve(n)) return false;
    for (size_t i = count; i < n; ++i) (*this)[i] = value;
    count = n;
    return true;
  }
  bool push_back(const T& value) {
    if (count == capacity && !reserve(external ? (capacity ? capacity * 2 : BLOCK_ITEMS) : capacity + BLOCK_ITEMS)) {
      return false;
    }
    (*this)[count++] = value;
    return true;
  }
  void clear() {
    buffer.reset();
    for (size_t i = 0; i < blockCount; ++i) delete[] blocks[i];
    blocks.reset();
    count = capacity = blockCount = 0;
  }
};
