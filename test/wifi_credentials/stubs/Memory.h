#pragma once
#include <memory>
#include <new>
template <class T>
std::unique_ptr<T> makeUniqueNoThrow(size_t size) {
  return std::unique_ptr<T>(new (std::nothrow) std::remove_extent_t<T>[size]);
}
