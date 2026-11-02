#pragma once
#include "esp_eap_client.h"
#include "esp_wifi_types.h"
constexpr int WIFI_IF_STA = 0;
struct wifi_config_t {
  struct {
    struct {
      wifi_auth_mode_t authmode;
    } threshold;
  } sta;
};
inline int esp_wifi_get_config(int, wifi_config_t*) { return 0; }
inline int esp_wifi_set_config(int, wifi_config_t* config) {
  return config->sta.threshold.authmode == WIFI_AUTH_WPA2_ENTERPRISE ? 0 : -1;
}
inline int esp_wifi_connect() {
  host_eap::calls.push_back("connect");
  return 0;
}
