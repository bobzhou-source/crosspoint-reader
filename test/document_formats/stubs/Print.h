#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

class Print {
 public:
  virtual ~Print() = default;
  virtual void flush() {}
  virtual size_t write(uint8_t byte) = 0;
  virtual size_t write(const uint8_t* data, size_t size) {
    size_t written = 0;
    while (written < size && write(data[written])) ++written;
    return written;
  }
  size_t print(const char* text) { return write(reinterpret_cast<const uint8_t*>(text), strlen(text)); }
};
