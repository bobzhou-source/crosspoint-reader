// Host probe of SecureClient's wolfSSL setup; sends no HTTP requests.
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/memory.h>
#include "TrustAnchors.generated.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
struct alignas(std::max_align_t) Allocation { size_t size; };
size_t liveBytes = 0, peakBytes = 0, largestAllocation = 0;
void* trackedMalloc(size_t size) {
  auto* block = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + size));
  if (!block) return nullptr;
  block->size = size;
  liveBytes += size;
  peakBytes = std::max(peakBytes, liveBytes);
  largestAllocation = std::max(largestAllocation, size);
  return block + 1;
}
void trackedFree(void* pointer) {
  if (!pointer) return;
  auto* block = static_cast<Allocation*>(pointer) - 1;
  liveBytes -= block->size;
  std::free(block);
}
void* trackedRealloc(void* pointer, size_t size) {
  if (!pointer) return trackedMalloc(size);
  if (!size) { trackedFree(pointer); return nullptr; }
  auto* old = static_cast<Allocation*>(pointer) - 1;
  const size_t oldSize = old->size;
  auto* block = static_cast<Allocation*>(std::realloc(old, sizeof(Allocation) + size));
  if (!block) return nullptr;
  block->size = size;
  liveBytes = liveBytes - oldSize + size;
  peakBytes = std::max(peakBytes, liveBytes);
  largestAllocation = std::max(largestAllocation, size);
  return block + 1;
}
int receive(WOLFSSL*, char* data, int size, void* context) {
  const int count = recv(*static_cast<int*>(context), data, size, 0);
  if (count > 0) return count;
  if (!count) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
             ? WOLFSSL_CBIO_ERR_WANT_READ : WOLFSSL_CBIO_ERR_GENERAL;
}
int sendData(WOLFSSL*, char* data, int size, void* context) {
  const int count = send(*static_cast<int*>(context), data, size, 0);
  if (count > 0) return count;
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
             ? WOLFSSL_CBIO_ERR_WANT_WRITE : WOLFSSL_CBIO_ERR_CONN_CLOSE;
}
int connectTcp(const char* host, const char* port) {
  addrinfo hints{}, *addresses = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_INET;
  if (getaddrinfo(host, port, &hints, &addresses)) return -1;
  int fd = -1;
  for (auto* address = addresses; address; address = address->ai_next) {
    fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) continue;
    timeval timeout{4, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (!connect(fd, address->ai_addr, address->ai_addrlen)) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(addresses);
  if (fd >= 0) fcntl(fd, F_SETFL, O_NONBLOCK);
  return fd;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: probe HOST [auto|tls12] [PORT] | --cert DER_FILE\n");
    return 2;
  }
  const bool certificate = std::strcmp(argv[1], "--cert") == 0;
  if (certificate && argc != 3) return 2;
  const bool tls12 = !certificate && argc > 2 && std::strcmp(argv[2], "tls12") == 0;
  wolfSSL_SetAllocators(trackedMalloc, trackedFree, trackedRealloc);
  wolfSSL_Init();
  if (std::getenv("TLS_DEBUG")) wolfSSL_Debugging_ON();
  auto* ctx = wolfSSL_CTX_new(tls12 ? wolfTLSv1_2_client_method() : wolfSSLv23_client_method());
  if (!ctx) return 2;
  wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, nullptr);
  int result = WOLFSSL_SUCCESS;
  size_t index = 0;
  for (const auto& root : freeink::trust::ROOTS) {
    result = wolfSSL_CTX_load_verify_buffer(ctx, root.data, root.size, WOLFSSL_FILETYPE_ASN1);
    if (result != WOLFSSL_SUCCESS) {
      std::printf("ROOT index=%zu result=%d\n", index, result);
      break;
    }
    ++index;
  }
  std::printf("ROOTS loaded=%zu live=%zu\n", index, liveBytes);
  int error = result == WOLFSSL_SUCCESS ? 0 : result;
  if (!error && certificate) {
    unsigned char der[16384];
    FILE* file = std::fopen(argv[2], "rb");
    if (!file) return 2;
    const size_t size = std::fread(der, 1, sizeof(der), file);
    const bool complete = std::fgetc(file) == EOF;
    std::fclose(file);
    if (!complete) return 2;
    result = wolfSSL_CertManagerVerifyBuffer(wolfSSL_CTX_GetCertManager(ctx), der, size, WOLFSSL_FILETYPE_ASN1);
    error = result == WOLFSSL_SUCCESS ? 0 : result;
    std::printf("CERT file=%s result=%d error=%d\n", argv[2], result, error);
  } else if (!error) {
    int fd = connectTcp(argv[1], argc > 3 ? argv[3] : "443");
    if (fd < 0) { std::fprintf(stderr, "TCP failed\n"); return 2; }
    wolfSSL_SetIORecv(ctx, receive);
    wolfSSL_SetIOSend(ctx, sendData);
    auto* ssl = wolfSSL_new(ctx);
    if (!ssl) return 2;
    wolfSSL_SetIOReadCtx(ssl, &fd);
    wolfSSL_SetIOWriteCtx(ssl, &fd);
    if (wolfSSL_check_domain_name(ssl, argv[1]) != WOLFSSL_SUCCESS ||
        wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, argv[1], std::strlen(argv[1])) != WOLFSSL_SUCCESS ||
        wolfSSL_UseKeyShare(ssl, WOLFSSL_ECC_X25519) != WOLFSSL_SUCCESS ||
        wolfSSL_UseMaxFragment(ssl, WOLFSSL_MFL_2_11) != WOLFSSL_SUCCESS) return 2;
    const auto start = std::chrono::steady_clock::now();
    do {
      result = wolfSSL_connect(ssl);
      error = result == WOLFSSL_SUCCESS ? 0 : wolfSSL_get_error(ssl, result);
      if (error != WOLFSSL_ERROR_WANT_READ && error != WOLFSSL_ERROR_WANT_WRITE) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() - start < std::chrono::seconds(4));
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("TLS host=%s mode=%s result=%d error=%d reason=%s version=%s cipher=%s ms=%lld\n",
                argv[1], tls12 ? "tls12" : "auto", result, error,
                error ? wolfSSL_ERR_reason_error_string(error) : "success",
                wolfSSL_get_version(ssl), wolfSSL_get_cipher(ssl), static_cast<long long>(milliseconds));
    wolfSSL_free(ssl);
    close(fd);
  }
  wolfSSL_CTX_free(ctx);
  wolfSSL_Cleanup();
  std::printf("HEAP peak=%zu largest=%zu residual=%zu (wolfSSL requested bytes; excludes allocator overhead/TCP/stack)\n",
              peakBytes, largestAllocation, liveBytes);
  return error ? 1 : 0;
}
