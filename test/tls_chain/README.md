# SecureNet public-chain probe

This opt-in network regression builds the installed **Arduino-wolfSSL 5.7.2**
sources, including its patched `user_settings.h`, with the crypto defines read
from `platformio.ini`. It loads the same generated DER trust anchors and mirrors
SecureClient's peer/hostname verification, SNI, X25519 key share, 2KB maximum
fragment request and four-second handshake deadline. It sends **no HTTP
requests**. Keep it outside ordinary `ctest`: public endpoints and certificate
issuance change independently of this repository.

Prerequisite: install/build the `x4pro-gh_release` PlatformIO dependencies. Then:

```sh
python3 test/tls_chain/build_host.py /tmp/tls-baseline --baseline
python3 test/tls_chain/build_host.py /tmp/tls-fixed
/tmp/tls-baseline/probe robertzhou.dev
/tmp/tls-fixed/probe robertzhou.dev
/tmp/tls-fixed/probe robertzhou.dev tls12
/tmp/tls-baseline/probe valid.x1.test-certs.letsencrypt.org
/tmp/tls-fixed/probe valid.x1.test-certs.letsencrypt.org
/tmp/tls-fixed/probe valid.x2.test-certs.letsencrypt.org
/tmp/tls-fixed/probe untrusted-root.badssl.com
/tmp/tls-fixed/probe self-signed.badssl.com
/tmp/tls-fixed/probe wrong.host.badssl.com
/tmp/tls-fixed/probe expired.badssl.com
```

Exit 0 means validated; exit 1 means certificate/handshake failure (inspect the
error, rather than treating a timeout as a successful negative test); exit 2
means harness/setup/transport failure. `TLS_DEBUG=1` enables wolfSSL's trace.
Optional third positional argument sets the TCP port after `auto` or `tls12`.
`probe --cert path.der` verifies a certificate against the same root store; it
does not install that certificate as a trust anchor.

## Firmware requirements

- `WOLFSSL_SHA384`: GTS WE1 and ISRG X2 paths require SHA-384. Its implementation
  shares `sha512.c`, but `WOLFSSL_SHA512` is not required for these chains.
- `WOLFSSL_SP_384`: uses small single-precision P-384 operations instead of the
  much larger generic fast-math allocations. The existing Arduino settings
  already enable all ECC curves; this is not a missing `HAVE_ECC384` fix.
- `WOLFSSL_ALT_CERT_CHAINS`: accepts a valid path to an existing trusted anchor
  even when a peer appends a cross-signed root with an unknown issuer. It does
  **not** trust that unknown issuer or disable leaf/hostname/date verification.
  Without it the GTS R4 cross-sign fails before WE1's signature is checked.

The probe's TLS 1.2 mode exercises SecureClient's retry method separately from
automatic TLS 1.3 negotiation; it does not modify the production fallback logic.

## Measurement limits

The host substitutes BSD I/O, OS entropy and wall clock for Arduino/lwIP and
`esp_random`. It preserves Arduino's timing-resistant crypto settings and
forces 32-bit SP/TFM digits, avoiding the host's faster arm64 math. Pointers and
some structures are still host-sized. `manifest.json` records the flags and
source/settings/root hashes for each build.

Allocator callbacks report requested-byte live peak, largest allocation and
post-cleanup residual. They exclude allocator metadata, stack, libc/socket
buffers and the device's Wi-Fi/RTOS heap/fragmentation; realloc records requested
live bytes, not any temporary libc copy. These are comparable host measurements,
**not** ESP32 free-heap or stack-high-water proof. Preserve the device's existing
heap guards and measure on-device before claiming memory headroom or timing.

## Rotation coverage

The store contains ISRG X1/X2 and GTS R1/R3/R4. Public Let's Encrypt intermediate
certificates (including both E5/E6 cross-sign variants) are available at
https://letsencrypt.org/certificates/ and can be checked with `--cert`. Current
Let's Encrypt test hosts may use successor YE/YR chains; capture their chain
when recording a result instead of labeling it E5/E6 by assumption.

Cloudflare can also issue through SSL.com:
https://developers.cloudflare.com/ssl/reference/certificate-authorities/ .
The present store has **no SSL.com or Certum roots**, so those paths must still
fail closed. `test-dv-ecc.ssl.com` and `test-dv-rsa.ssl.com` are public probes
listed by https://www.ssl.com/sample-valid-revoked-and-expired-ssl-tls-certificates/ .
This crypto fix does not silently expand the trust store. It also does not add
SHA-512 signature support or revocation checking.
