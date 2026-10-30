#pragma once
#include "Client.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

// Only Arduino TCP/timing APIs are adapted; production SecureClient and the
// vendored wolfSSL implementation perform all TLS and certificate checks.
class WiFiClient : public Client {
 public:
  ~WiFiClient() override { stop(); }
  void setConnectionTimeout(uint32_t) {}
  int connect(IPAddress, uint16_t) override { return 0; }
  int connect(const char*, uint16_t port) override {
    stop();
    fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { stop(); return 0; }
    fcntl(fd, F_SETFL, O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    return 1;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    const auto n = ::send(fd, data, size, 0);
    return n > 0 ? n : 0;
  }
  int read(uint8_t* data, size_t size) override { return ::recv(fd, data, size, 0); }
  int read() override { uint8_t b; return read(&b, 1) == 1 ? b : -1; }
  int peek() override { uint8_t b; return ::recv(fd, &b, 1, MSG_PEEK) == 1 ? b : -1; }
  int available() override { int bytes = 0; if (fd >= 0) ioctl(fd, FIONREAD, &bytes); return bytes; }
  uint8_t connected() override {
    if (fd < 0) return false;
    uint8_t b;
    return ::recv(fd, &b, 1, MSG_PEEK) != 0;
  }
  operator bool() override { return connected(); }
  void flush() override {}
  void stop() override { if (fd >= 0) ::close(fd); fd = -1; }
 private:
  int fd = -1;
};
