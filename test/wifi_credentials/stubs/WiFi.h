#pragma once
#include "esp_eap_client.h"
constexpr int WIFI_STA = 1, WL_CONNECT_FAILED = 4;
struct HostWifi {
  bool mode(int) { return true; }
  int begin(const char*, const char*, int = 0, const unsigned char* = nullptr, bool connect = true) {
    host_eap::calls.push_back(connect ? "psk-connect" : "configure");
    return 0;
  }
};
inline HostWifi WiFi;
