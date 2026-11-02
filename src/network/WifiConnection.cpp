#include "WifiConnection.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_eap_client.h>
#include <esp_wifi.h>

#include <cstring>
#include <memory>

#include "WifiCredentialStore.h"

namespace wifi_connection {
namespace {
// IDF keeps this pointer, including during reauthentication. Keep it until EAP is disabled.
std::unique_ptr<unsigned char[]> caCertificate;
constexpr size_t MAX_CA_BYTES = 16384;

bool configureEnterprise(const WifiCredential& credential) {
  if (credential.identity.empty() || credential.identity.size() > 127 || credential.password.empty() ||
      credential.password.size() > 127 || credential.anonymousIdentity.size() > 127)
    return false;
  // IDF 5.5's PEAP phase2 default is auth=MSCHAPV2 (eap_peer/eap.c).
  if (esp_eap_client_set_eap_methods(ESP_EAP_TYPE_PEAP) != ESP_OK ||
      esp_eap_client_use_default_cert_bundle(false) != ESP_OK || esp_eap_client_set_disable_time_check(false) != ESP_OK)
    return false;
  if (credential.validateServer) {
    // No implicit downgrade: a missing CA, name, clock or validation API fails closed.
    if (credential.domain.empty() || credential.domain.size() > 253 ||
        credential.caPath.compare(0, sizeof("/.crosspoint/wifi-ca/") - 1, "/.crosspoint/wifi-ca/") != 0 ||
        credential.caPath.find("..") != std::string::npos)
      return false;
    auto file = Storage.open(credential.caPath.c_str());
    if (!file || file.size() == 0 || file.size() > MAX_CA_BYTES) return false;
    const size_t length = file.size();
    caCertificate = makeUniqueNoThrow<unsigned char[]>(length + 1);
    if (!caCertificate || file.read(caCertificate.get(), length) != static_cast<int>(length)) return false;
    caCertificate[length] = 0;
    if (esp_eap_client_set_ca_cert(caCertificate.get(), length + 1) != ESP_OK ||
        esp_eap_client_set_domain_name(credential.domain.c_str()) != ESP_OK)
      return false;
  }
  const auto& outerIdentity = credential.anonymousIdentity.empty() ? credential.identity : credential.anonymousIdentity;
  return esp_eap_client_set_identity(reinterpret_cast<const unsigned char*>(outerIdentity.data()),
                                     outerIdentity.size()) == ESP_OK &&
         esp_eap_client_set_username(reinterpret_cast<const unsigned char*>(credential.identity.data()),
                                     credential.identity.size()) == ESP_OK &&
         esp_eap_client_set_password(reinterpret_cast<const unsigned char*>(credential.password.data()),
                                     credential.password.size()) == ESP_OK &&
         esp_wifi_sta_enterprise_enable() == ESP_OK;
}
}  // namespace

bool clearEnterprise() {
  if (esp_wifi_sta_enterprise_disable() != ESP_OK) return false;
  // disable resets all EAP configuration, including the certificate pointer.
  caCertificate.reset();
  return true;
}

bool begin(const WifiCredential& credential) {
  if (!WiFi.mode(WIFI_STA)) return false;
  if (!clearEnterprise()) return false;
  if (credential.ssid.empty() || credential.ssid.size() > 32) return false;
  if (!credential.enterprise) {
    return WiFi.begin(credential.ssid.c_str(), credential.password.empty() ? nullptr : credential.password.c_str()) !=
           WL_CONNECT_FAILED;
  }
  // Configure without connecting so EAP cannot race its credentials or certificate settings.
  if (WiFi.begin(credential.ssid.c_str(), nullptr, 0, nullptr, false) == WL_CONNECT_FAILED) return false;
  {
    // wifi_config_t exceeds the UI task's small stack budget; release before TLS starts.
    auto config = makeUniqueNoThrow<wifi_config_t[]>(1);
    if (!config || esp_wifi_get_config(WIFI_IF_STA, config.get()) != ESP_OK) return false;
    config[0].sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
    if (esp_wifi_set_config(WIFI_IF_STA, config.get()) != ESP_OK) return false;
  }
  if (!configureEnterprise(credential)) {
    LOG_ERR("WIFI", "Enterprise configuration failed (check identity, CA file, server name and clock)");
    clearEnterprise();
    return false;
  }
  if (esp_wifi_connect() != ESP_OK) {
    clearEnterprise();
    return false;
  }
  return true;
}
}  // namespace wifi_connection
