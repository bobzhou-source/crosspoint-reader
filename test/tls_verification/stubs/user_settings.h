#pragma once
// Portable equivalent of the firmware's security/TLS settings. No ESP32 crypto
// acceleration, Arduino hooks, or certificate/time/hostname bypasses.
#define NO_FILESYSTEM
#define NO_OLD_TLS
#define WOLFSSL_TLS13
#define WOLFSSL_CLIENT_EXAMPLE
#define NO_WOLFSSL_SERVER
#define HAVE_TLS_EXTENSIONS
#define HAVE_SUPPORTED_CURVES
#define HAVE_SNI
#define HAVE_MAX_FRAGMENT
#define WC_RSA_PSS
#define HAVE_HKDF
#define HAVE_AEAD
#define HAVE_AESGCM
#define HAVE_ECC
#define HAVE_CURVE25519
#define WOLFSSL_SMALL_STACK
#define WOLFSSL_LOW_MEMORY
#define MICRO_SESSION_CACHE
#define SINGLE_THREADED
#define RSA_LOW_MEM
#define NO_DSA
#define NO_DH
#define USE_FAST_MATH
#define FP_MAX_BITS 8192
#define WOLFSSL_HAVE_SP_ECC
#define WOLFSSL_SP_SMALL
#define ALT_ECC_SIZE
