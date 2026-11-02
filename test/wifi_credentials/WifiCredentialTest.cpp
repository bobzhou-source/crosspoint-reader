#include <HalStorage.h>
#include <ObfuscationUtils.h>
#include <esp_eap_client.h>
#include <gtest/gtest.h>

#include <algorithm>

#include "WifiCredentialStore.h"
#include "network/WifiAuthentication.h"
#include "network/WifiConnection.h"
#include "network/WifiFailure.h"

namespace {
WifiCredential enterprise() {
  WifiCredential c;
  c.ssid = "Campus";
  c.enterprise = true;
  c.identity = "fixture@example.edu";
  c.password = "synthetic-test-password";
  c.caPath = "/.crosspoint/wifi-ca/colby.pem";
  c.domain = "clearpass.colby.edu";
  return c;
}
class WifiCredentials : public testing::Test {
 protected:
  void SetUp() override {
    WIFI_STORE.clearAll();
    Storage.files.clear();
    host_eap::fail = false;
    wifi_connection::clearEnterprise();
    host_eap::calls.clear();
  }
};
TEST_F(WifiCredentials, LegacyPlaintextAndOpenLoadUnchanged) {
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(
      doc,
      R"({"lastConnectedSsid":"Home","credentials":[{"ssid":"Home","password":"old-password"},{"ssid":"Open","password":""}]})"));
  ASSERT_TRUE(WIFI_STORE.fromJson(doc.as<JsonVariantConst>()));
  EXPECT_EQ(WIFI_STORE.getCredentialCount(), 2u);
  EXPECT_EQ(WIFI_STORE.findCredential("Home")->password, "old-password");
  EXPECT_FALSE(WIFI_STORE.findCredential("Home")->enterprise);
  EXPECT_TRUE(WIFI_STORE.findCredential("Open")->password.empty());
  EXPECT_EQ(WIFI_STORE.getLastConnectedSsid(), "Home");
}
TEST_F(WifiCredentials, EnterpriseRoundTripKeepsLegacyArrayPskOnly) {
  ASSERT_TRUE(WIFI_STORE.addCredential("Home", "psk-fixture"));
  auto c = enterprise();
  c.anonymousIdentity = "anonymous@example.edu";
  ASSERT_TRUE(WIFI_STORE.addCredential(c));
  WIFI_STORE.setLastConnectedSsid(c.ssid);
  JsonDocument doc;
  WIFI_STORE.toJson(doc);
  ASSERT_EQ(doc["credentials"].size(), 1u);
  EXPECT_STREQ(doc["credentials"][0]["ssid"], "Home");
  EXPECT_TRUE(doc["credentials"][0]["password"].isNull());
  ASSERT_EQ(doc["enterpriseCredentials"].size(), 1u);
  EXPECT_STREQ(doc["enterpriseCredentials"][0]["type"], "enterprise");
  EXPECT_STREQ(doc["enterpriseCredentials"][0]["eap"], "PEAP");
  EXPECT_STREQ(doc["enterpriseCredentials"][0]["phase2"], "MSCHAPv2");
  EXPECT_TRUE(doc["enterpriseCredentials"][0]["password"].isNull());
  ASSERT_TRUE(WIFI_STORE.fromJson(doc.as<JsonVariantConst>()));
  auto loaded = WIFI_STORE.findCredential(c.ssid);
  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->password, c.password);
  EXPECT_EQ(loaded->identity, c.identity);
  EXPECT_EQ(loaded->anonymousIdentity, c.anonymousIdentity);
  EXPECT_EQ(loaded->caPath, c.caPath);
  EXPECT_EQ(loaded->domain, c.domain);
  EXPECT_TRUE(loaded->validateServer);
  EXPECT_EQ(WIFI_STORE.getLastConnectedSsid(), c.ssid);
  EXPECT_EQ(WIFI_STORE.findCredential("Home")->password, "psk-fixture");
}
TEST_F(WifiCredentials, ExplicitNoValidationPersistsButMissingDefaultsSecure) {
  auto c = enterprise();
  c.validateServer = false;
  ASSERT_TRUE(WIFI_STORE.addCredential(c));
  JsonDocument doc;
  WIFI_STORE.toJson(doc);
  ASSERT_TRUE(WIFI_STORE.fromJson(doc.as<JsonVariantConst>()));
  EXPECT_FALSE(WIFI_STORE.findCredential(c.ssid)->validateServer);
  doc["enterpriseCredentials"][0].remove("validation");
  ASSERT_TRUE(WIFI_STORE.fromJson(doc.as<JsonVariantConst>()));
  EXPECT_TRUE(WIFI_STORE.findCredential(c.ssid)->validateServer);
}
TEST_F(WifiCredentials, CorruptedEnterprisePasswordIsRejectedWithoutDroppingPsk) {
  ASSERT_TRUE(WIFI_STORE.addCredential("Home", "psk-fixture"));
  ASSERT_TRUE(WIFI_STORE.addCredential(enterprise()));
  JsonDocument doc;
  WIFI_STORE.toJson(doc);
  doc["enterpriseCredentials"][0]["password_crc32"] = 0;
  ASSERT_TRUE(WIFI_STORE.fromJson(doc.as<JsonVariantConst>()));
  EXPECT_EQ(WIFI_STORE.getCredentialCount(), 1u);
  EXPECT_TRUE(WIFI_STORE.findCredential("Home"));
}
TEST_F(WifiCredentials, UnsupportedEapAndOversizedIdentityAreRejected) {
  ASSERT_TRUE(WIFI_STORE.addCredential(enterprise()));
  JsonDocument doc;
  WIFI_STORE.toJson(doc);
  doc["enterpriseCredentials"][0]["eap"] = "TLS";
  ASSERT_TRUE(WIFI_STORE.fromJson(doc.as<JsonVariantConst>()));
  EXPECT_EQ(WIFI_STORE.getCredentialCount(), 0u);
  auto c = enterprise();
  c.identity.assign(128, 'x');
  EXPECT_FALSE(WIFI_STORE.addCredential(c));
}
TEST_F(WifiCredentials, EnterprisePasswordLimitDoesNotChangePskLimit) {
  auto c = enterprise();
  c.password.assign(127, 'x');
  ASSERT_TRUE(WIFI_STORE.addCredential(c));
  JsonDocument doc;
  WIFI_STORE.toJson(doc);
  ASSERT_TRUE(WIFI_STORE.fromJson(doc.as<JsonVariantConst>()));
  EXPECT_EQ(WIFI_STORE.findCredential(c.ssid)->password.size(), 127u);
  EXPECT_FALSE(WIFI_STORE.addCredential("Home", std::string(65, 'x')));
}
TEST_F(WifiCredentials, AuthModesDistinguishEnterpriseAndSuiteB) {
  for (auto mode : {WIFI_AUTH_WPA2_ENTERPRISE, WIFI_AUTH_WPA3_ENTERPRISE, WIFI_AUTH_WPA2_WPA3_ENTERPRISE,
                    WIFI_AUTH_WPA_ENTERPRISE}) {
    EXPECT_TRUE(wifi_authentication::isEnterprise(mode));
    EXPECT_TRUE(wifi_authentication::supportsPeap(mode));
  }
  EXPECT_TRUE(wifi_authentication::isEnterprise(WIFI_AUTH_WPA3_ENT_192));
  EXPECT_FALSE(wifi_authentication::supportsPeap(WIFI_AUTH_WPA3_ENT_192));
  for (auto mode : {WIFI_AUTH_OPEN, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA3_PSK,
                    WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_OWE, WIFI_AUTH_DPP})
    EXPECT_FALSE(wifi_authentication::isEnterprise(mode));
}
TEST_F(WifiCredentials, MissingCaFailsClosedBeforeAssociation) {
  EXPECT_FALSE(wifi_connection::begin(enterprise()));
  EXPECT_FALSE(host_eap::enabled);
  EXPECT_EQ(std::count(host_eap::calls.begin(), host_eap::calls.end(), "connect"), 0);
}
TEST_F(WifiCredentials, ValidatedSetupRetainsCaAndUsesExactServerName) {
  auto c = enterprise();
  c.anonymousIdentity = "anonymous@example.edu";
  Storage.files[c.caPath] = "synthetic-ca-bytes";
  ASSERT_TRUE(wifi_connection::begin(c));
  EXPECT_EQ(host_eap::identity, c.anonymousIdentity);
  EXPECT_EQ(host_eap::username, c.identity);
  EXPECT_EQ(host_eap::password, c.password);
  EXPECT_EQ(host_eap::domain, c.domain);
  EXPECT_STREQ(reinterpret_cast<const char*>(host_eap::ca), "synthetic-ca-bytes");
  EXPECT_TRUE(host_eap::timeCheck);
  EXPECT_EQ(host_eap::calls, (std::vector<std::string>{"disable", "configure", "peap", "enable", "connect"}));
}
TEST_F(WifiCredentials, PskAndOpenClearEnterpriseSecretsAndCa) {
  auto c = enterprise();
  c.validateServer = false;
  ASSERT_TRUE(wifi_connection::begin(c));
  WifiCredential home;
  home.ssid = "Home";
  home.password = "psk-fixture";
  ASSERT_TRUE(wifi_connection::begin(home));
  EXPECT_FALSE(host_eap::enabled);
  EXPECT_TRUE(host_eap::username.empty());
  EXPECT_TRUE(host_eap::password.empty());
  EXPECT_TRUE(host_eap::domain.empty());
  EXPECT_EQ(host_eap::ca, nullptr);
  home.password.clear();
  ASSERT_TRUE(wifi_connection::begin(home));
  EXPECT_EQ(host_eap::calls.back(), "psk-connect");
}
TEST_F(WifiCredentials, ExplicitInsecureModeAndApiFailureNeverSilentlyDowngrade) {
  auto c = enterprise();
  c.validateServer = false;
  ASSERT_TRUE(wifi_connection::begin(c));
  EXPECT_EQ(host_eap::ca, nullptr);
  EXPECT_TRUE(host_eap::domain.empty());
  host_eap::fail = true;
  EXPECT_FALSE(wifi_connection::begin(c));
  EXPECT_FALSE(host_eap::enabled);
}
TEST_F(WifiCredentials, DisconnectReasonsAndDhcpHaveDistinctHumanCategories) {
  using wifi_connection::classifyFailure;
  using wifi_connection::Failure;
  EXPECT_EQ(classifyFailure(201, false, false), Failure::NotFound);
  EXPECT_EQ(classifyFailure(202, false, false), Failure::Rejected);
  EXPECT_EQ(classifyFailure(203, false, false), Failure::Rejected);
  EXPECT_EQ(classifyFailure(15, false, false), Failure::Handshake);
  EXPECT_EQ(classifyFailure(204, false, false), Failure::Handshake);
  EXPECT_EQ(classifyFailure(23, false, false), Failure::Enterprise);
  EXPECT_EQ(classifyFailure(210, false, false), Failure::Security);
  EXPECT_EQ(classifyFailure(0, true, false), Failure::NoIp);
  EXPECT_EQ(classifyFailure(0, false, false), Failure::Timeout);
  EXPECT_EQ(classifyFailure(200, true, false), Failure::Disconnected);
  EXPECT_EQ(classifyFailure(1, true, true), Failure::Disconnected);
}
}  // namespace
