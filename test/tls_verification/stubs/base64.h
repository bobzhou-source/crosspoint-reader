#pragma once
#include <string>
namespace base64 {
inline std::string encode(const char* input) {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  unsigned bits = 0;
  int count = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(input); *p; ++p) {
    bits = (bits << 8) | *p;
    count += 8;
    while (count >= 6) { count -= 6; output += alphabet[(bits >> count) & 63]; }
  }
  if (count) output += alphabet[(bits << (6 - count)) & 63];
  while (output.size() % 4) output += '=';
  return output;
}
}
