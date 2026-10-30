#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
using String = std::string;
inline unsigned long millis() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}
inline void delay(unsigned long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
struct SerialAdapter {
  operator bool() const { return true; }
  template <typename... Args> void printf(const char* format, Args... args) { std::printf(format, args...); }
  void println(const char* text) { std::puts(text); }
};
inline SerialAdapter Serial;
struct EspAdapter {
  unsigned getFreeHeap() const { return 0; }
  unsigned getMaxAllocHeap() const { return 0; }
};
inline EspAdapter ESP;
class IPAddress {
 public:
  uint8_t operator[](int) const { return 0; }
};
