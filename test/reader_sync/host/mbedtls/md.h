#pragma once
#include <openssl/hmac.h>
#include <cstdint>
#include <cstddef>
constexpr int MBEDTLS_MD_SHA256 = 1;
inline const EVP_MD* mbedtls_md_info_from_type(int) { return EVP_sha256(); }
inline int mbedtls_md_hmac(const EVP_MD* type, const uint8_t* key, size_t keySize, const uint8_t* message, size_t messageSize, uint8_t* out) {
  unsigned size = 32;
  return HMAC(type, key, static_cast<int>(keySize), message, messageSize, out, &size) && size == 32 ? 0 : -1;
}
