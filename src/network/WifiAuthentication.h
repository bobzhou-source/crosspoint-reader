#pragma once

#include <esp_wifi_types.h>

namespace wifi_authentication {
constexpr bool isEnterprise(wifi_auth_mode_t mode) {
  return mode == WIFI_AUTH_WPA2_ENTERPRISE || mode == WIFI_AUTH_WPA3_ENTERPRISE ||
         mode == WIFI_AUTH_WPA2_WPA3_ENTERPRISE || mode == WIFI_AUTH_WPA3_ENT_192 || mode == WIFI_AUTH_WPA_ENTERPRISE;
}
constexpr bool supportsPeap(wifi_auth_mode_t mode) { return isEnterprise(mode) && mode != WIFI_AUTH_WPA3_ENT_192; }
}  // namespace wifi_authentication
