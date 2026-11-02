#pragma once
#include <cstddef>
#include <cstring>
#define MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL -42
inline int mbedtls_base64_decode(unsigned char* dst, size_t cap, size_t* out, const unsigned char* src, size_t len) {
  constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (len % 4) return -44;
  *out = len / 4 * 3 - (len && src[len - 1] == '=') - (len > 1 && src[len - 2] == '=');
  for (size_t i = 0; i < len; ++i)
    if (src[i] != '=' && !strchr(alphabet, src[i])) return -44;
  if (!dst || cap < *out) return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
  size_t j = 0;
  for (size_t i = 0; i < len; i += 4) {
    unsigned n = 0;
    for (size_t k = 0; k < 4; ++k) n = (n << 6) | (src[i + k] == '=' ? 0 : strchr(alphabet, src[i + k]) - alphabet);
    for (int shift = 16; shift >= 0 && j < *out; shift -= 8) dst[j++] = (n >> shift) & 255;
  }
  return 0;
}
