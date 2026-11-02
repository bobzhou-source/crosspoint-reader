#pragma once

struct WifiCredential;
namespace wifi_connection {
// Caller owns the station until disconnected; no concurrent foreground/background owners.
bool begin(const WifiCredential& credential);
bool clearEnterprise();
}  // namespace wifi_connection
