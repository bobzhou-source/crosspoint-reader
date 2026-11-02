#pragma once
#include <cstdint>

namespace wifi_connection {
enum class Failure { NotFound, Rejected, Handshake, Enterprise, NoIp, Security, Timeout, Disconnected };
// IEEE 802.11 / ESP-IDF wifi_err_reason_t values; kept platform-free for host tests.
constexpr Failure classifyFailure(uint16_t reason, bool associated, bool gotIp) {
  switch (reason) {
    case 201:
    case 212:
      return Failure::NotFound;
    case 5:
    case 6:
    case 7:
    case 9:
    case 30:
    case 202:
    case 203:
      return Failure::Rejected;
    case 15:
    case 16:
    case 204:
      return Failure::Handshake;
    case 23:
      return Failure::Enterprise;
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 24:
    case 29:
    case 210:
    case 211:
      return Failure::Security;
    default:
      if (associated && !gotIp && reason == 0) return Failure::NoIp;
      return reason ? Failure::Disconnected : Failure::Timeout;
  }
}
}  // namespace wifi_connection
