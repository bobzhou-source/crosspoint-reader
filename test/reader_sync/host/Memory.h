#pragma once
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
template<class T, class... Args>
std::enable_if_t<!std::is_array_v<T>, std::unique_ptr<T>> makeUniqueNoThrow(Args&&... args) {
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}
template<class T>
std::enable_if_t<std::is_array_v<T>, std::unique_ptr<T>> makeUniqueNoThrow(size_t n) {
  return std::unique_ptr<T>(new (std::nothrow) std::remove_extent_t<T>[n]{});
}
