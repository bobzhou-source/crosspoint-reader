#pragma once
#include <cstdint>
#include <string>
class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  bool isEmpty() const { return empty(); }
  size_t write(uint8_t c) {
    push_back(static_cast<char>(c));
    return 1;
  }
  size_t write(const uint8_t* bytes, size_t count) {
    append(reinterpret_cast<const char*>(bytes), count);
    return count;
  }
};
