#pragma once

#include <cstddef>
#include <string>

// Arduino String accepts C strings, not an implicit std::string conversion.
// Keeping that boundary makes real FsHelpers overload resolution match firmware.
class String {
 public:
  String() = default;
  String(const char* value) : value_(value ? value : "") {}

  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.length(); }

 private:
  std::string value_;
};
