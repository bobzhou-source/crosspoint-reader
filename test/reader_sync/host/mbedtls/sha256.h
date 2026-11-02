#pragma once
#include <openssl/sha.h>
#include <cstdint>
#include <cstddef>
using mbedtls_sha256_context = SHA256_CTX;
inline void mbedtls_sha256_init(mbedtls_sha256_context*) {}
inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* c, int) { return SHA256_Init(c) == 1 ? 0 : -1; }
inline int mbedtls_sha256_update(mbedtls_sha256_context* c, const uint8_t* p, size_t n) { return SHA256_Update(c, p, n) == 1 ? 0 : -1; }
inline int mbedtls_sha256_finish(mbedtls_sha256_context* c, uint8_t* p) { return SHA256_Final(p, c) == 1 ? 0 : -1; }
inline int mbedtls_sha256(const uint8_t* p, size_t n, uint8_t* out, int) { return SHA256(p, n, out) ? 0 : -1; }
