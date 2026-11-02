#pragma once
// ReaderSyncStorage.cpp includes ReaderSyncProtocol.h only for its declarations
// and the private-path policy. The privacy regression does not compile or link
// the network protocol, so the client type only needs to exist for the class
// declaration in that header. No definitions, credentials or network are used.
#include <cstddef>
#include <cstdint>
#include <functional>

namespace freeink {
class SecureHttpClient {
 public:
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;
};
}  // namespace freeink
