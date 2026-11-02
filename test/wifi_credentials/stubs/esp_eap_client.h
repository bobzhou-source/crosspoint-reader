#pragma once
#include <string>
#include <vector>
constexpr int ESP_OK = 0;
constexpr int ESP_EAP_TYPE_PEAP = 4;
namespace host_eap {
inline bool enabled = false, fail = false, timeCheck = false;
inline std::string identity, username, password, domain;
inline const unsigned char* ca = nullptr;
inline std::vector<std::string> calls;
}  // namespace host_eap
inline int esp_wifi_sta_enterprise_disable() {
  using namespace host_eap;
  calls.push_back("disable");
  enabled = false;
  identity.clear();
  username.clear();
  password.clear();
  domain.clear();
  ca = nullptr;
  return ESP_OK;
}
inline int esp_eap_client_set_eap_methods(int mask) {
  host_eap::calls.push_back("peap");
  return mask == 4 && !host_eap::fail ? 0 : -1;
}
inline int esp_eap_client_use_default_cert_bundle(bool enabled) { return enabled ? -1 : 0; }
inline int esp_eap_client_set_disable_time_check(bool disabled) {
  host_eap::timeCheck = !disabled;
  return 0;
}
inline int esp_eap_client_set_ca_cert(const unsigned char* ca, int) {
  host_eap::ca = ca;
  return 0;
}
inline int esp_eap_client_set_domain_name(const char* domain) {
  host_eap::domain = domain;
  return 0;
}
inline int esp_eap_client_set_identity(const unsigned char* s, int n) {
  host_eap::identity.assign(reinterpret_cast<const char*>(s), n);
  return 0;
}
inline int esp_eap_client_set_username(const unsigned char* s, int n) {
  host_eap::username.assign(reinterpret_cast<const char*>(s), n);
  return 0;
}
inline int esp_eap_client_set_password(const unsigned char* s, int n) {
  host_eap::password.assign(reinterpret_cast<const char*>(s), n);
  return 0;
}
inline int esp_wifi_sta_enterprise_enable() {
  host_eap::enabled = true;
  host_eap::calls.push_back("enable");
  return 0;
}
