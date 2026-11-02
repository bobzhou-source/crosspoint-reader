#pragma once
#include "Arduino.h"
namespace base64 {
inline String encode(const uint8_t* bytes, size_t size) {
  constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  for (size_t i = 0; i < size; i += 3) {
    uint32_t n = uint32_t(bytes[i]) << 16;
    if (i + 1 < size) n |= uint32_t(bytes[i + 1]) << 8;
    if (i + 2 < size) n |= bytes[i + 2];
    out += alphabet[(n >> 18) & 63];
    out += alphabet[(n >> 12) & 63];
    out += i + 1 < size ? alphabet[(n >> 6) & 63] : '=';
    out += i + 2 < size ? alphabet[n & 63] : '=';
  }
  return out;
}
}  // namespace base64
