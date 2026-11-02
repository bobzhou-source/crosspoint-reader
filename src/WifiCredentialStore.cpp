#include "WifiCredentialStore.h"

#include <CredentialIntegrity.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>

void WifiCredentialStore::toJson(JsonDocument& doc) const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  doc["lastConnectedSsid"] = lastConnectedSsid;

  JsonArray arr = doc["credentials"].to<JsonArray>();
  JsonArray enterprise = doc["enterpriseCredentials"].to<JsonArray>();
  for (const auto& cred : credentials) {
    JsonObject obj = (cred.enterprise ? enterprise : arr).add<JsonObject>();
    if (cred.enterprise) {
      obj["type"] = "enterprise";
      obj["eap"] = "PEAP";
      obj["phase2"] = "MSCHAPv2";
      obj["identity"] = cred.identity;
      obj["anonymous_identity"] = cred.anonymousIdentity;
      obj["ca_path"] = cred.caPath;
      obj["domain"] = cred.domain;
      obj["validation"] = cred.validateServer ? "ca" : "none";
    }
    obj["ssid"] = cred.ssid;
    obj["password_obf"] = obfuscation::obfuscateToBase64(cred.password);
    // XOR and base64 must round-trip to exactly this many bytes. Keeping the
    // original length lets the loader reject a decodable-but-corrupted value
    // instead of silently trying a different password.
    obj["password_len"] = cred.password.size();
    obj["password_crc32"] = credential_integrity::crc32(cred.password);
  }
}

bool WifiCredentialStore::fromJson(JsonVariantConst doc) {
  std::lock_guard<std::mutex> lock(credentialMutex);
  lastConnectedSsid = doc["lastConnectedSsid"] | "";

  // Tolerate a missing/invalid 'credentials' key (treat as empty list); only
  // a JSON parse error is fatal. A null JsonArray iterates zero times.
  credentials.clear();
  JsonArrayConst arr = doc["credentials"].as<JsonArrayConst>();
  const JsonArrayConst enterprise = doc["enterpriseCredentials"].as<JsonArrayConst>();
  credentials.reserve(std::min(arr.size() + enterprise.size(), MAX_NETWORKS));
  bool needsResave = false;

  // Older firmware sees only PSK/open entries, never an enterprise password as a PSK.
  for (int kind = 0; kind < 2; ++kind) {
    const JsonArrayConst entries = kind == 0 ? arr : enterprise;
    for (JsonObjectConst obj : entries) {
      if (credentials.size() >= MAX_NETWORKS) break;
      WifiCredential cred;
      cred.ssid = obj["ssid"] | "";
      cred.enterprise = kind == 1;
      const size_t maxPassword = cred.enterprise ? MAX_ENTERPRISE_PASSWORD_LENGTH : MAX_PASSWORD_LENGTH;
      if (cred.enterprise) {
        if (std::string(obj["type"] | "") != "enterprise" || std::string(obj["eap"] | "") != "PEAP" ||
            std::string(obj["phase2"] | "") != "MSCHAPv2")
          continue;
        cred.identity = obj["identity"] | "";
        cred.anonymousIdentity = obj["anonymous_identity"] | "";
        cred.caPath = obj["ca_path"] | "";
        cred.domain = obj["domain"] | "";
        const std::string validation = obj["validation"] | "ca";
        if (validation != "ca" && validation != "none") continue;
        cred.validateServer = validation != "none";
        if (cred.identity.empty() || cred.identity.size() > 127 || cred.anonymousIdentity.size() > 127 ||
            cred.domain.size() > 253 || cred.caPath.size() > 255)
          continue;
      } else if (std::string(obj["type"] | "psk") != "psk") {
        continue;
      }
      if (cred.ssid.empty() || cred.ssid.size() > 32) continue;

      const JsonVariantConst passwordLength = obj["password_len"];
      const bool hasPasswordLength = !passwordLength.isNull();
      size_t expectedLength = 0;
      if (hasPasswordLength) {
        if (!passwordLength.is<size_t>()) {
          LOG_ERR("WCS", "Discarding corrupted password for %s (invalid length)", cred.ssid.c_str());
          needsResave = true;
          continue;
        }
        expectedLength = passwordLength.as<size_t>();
        if (expectedLength > maxPassword) {
          LOG_ERR("WCS", "Discarding oversized password for %s (%zu bytes)", cred.ssid.c_str(), expectedLength);
          needsResave = true;
          continue;
        }
      }

      bool passwordValid = false;
      cred.password = extractPassword(obj, needsResave, maxPassword, passwordValid);
      if (!passwordValid) {
        LOG_ERR("WCS", "Discarding oversized password for %s", cred.ssid.c_str());
        needsResave = true;
        continue;
      }

      bool integrityValid = true;
      if (hasPasswordLength) {
        if (cred.password.size() != expectedLength) {
          LOG_ERR("WCS", "Discarding corrupted password for %s (expected %zu bytes, decoded %zu)", cred.ssid.c_str(),
                  expectedLength, cred.password.size());
          integrityValid = false;
        }
      } else {
        // Upgrade existing JSON after it has loaded successfully.
        needsResave = true;
      }

      const JsonVariantConst checksum = obj["password_crc32"];
      if (checksum.is<uint32_t>()) {
        const uint32_t expectedCrc32 = checksum.as<uint32_t>();
        if (credential_integrity::crc32(cred.password) != expectedCrc32) {
          LOG_ERR("WCS", "Discarding corrupted password for %s (checksum mismatch)", cred.ssid.c_str());
          integrityValid = false;
        }
      } else if (checksum.isNull()) {
        // password_crc32 was added after password_len; accept and upgrade files
        // written by older firmware.
        needsResave = true;
      } else {
        LOG_ERR("WCS", "Discarding corrupted password for %s (invalid checksum)", cred.ssid.c_str());
        integrityValid = false;
      }

      if (!integrityValid) {
        needsResave = true;
        continue;
      }
      if (cred.enterprise && cred.password.empty()) continue;
      credentials.push_back(std::move(cred));
    }
  }

  LOG_DBG("WCS", "Loaded %zu WiFi credentials from file", credentials.size());

  if (needsResave) {
    LOG_DBG("WCS", "Resaving JSON with obfuscated passwords");
    requestResave();
  }

  return true;
}

bool WifiCredentialStore::addCredential(const std::string& ssid, const std::string& password) {
  WifiCredential credential;
  credential.ssid = ssid;
  credential.password = password;
  return addCredential(credential);
}

bool WifiCredentialStore::addCredential(const WifiCredential& credential) {
  const auto& ssid = credential.ssid;
  if (ssid.empty() || ssid.size() > 32 ||
      credential.password.size() > (credential.enterprise ? MAX_ENTERPRISE_PASSWORD_LENGTH : MAX_PASSWORD_LENGTH) ||
      (credential.enterprise &&
       (credential.identity.empty() || credential.identity.size() > 127 || credential.password.empty() ||
        credential.anonymousIdentity.size() > 127 || credential.domain.size() > 253 || credential.caPath.size() > 255)))
    return false;
  {
    std::lock_guard<std::mutex> lock(credentialMutex);

    // Check if this SSID already exists and update it
    const auto cred = find_if(credentials.begin(), credentials.end(),
                              [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
    if (cred != credentials.end()) {
      *cred = credential;
      LOG_DBG("WCS", "Updated credentials for: %s", ssid.c_str());
    } else {
      // Check if we've reached the limit
      if (credentials.size() >= MAX_NETWORKS) {
        LOG_DBG("WCS", "Cannot add more networks, limit of %zu reached", MAX_NETWORKS);
        return false;
      }

      // Add new credential
      credentials.push_back(credential);
      LOG_DBG("WCS", "Added credentials for: %s", ssid.c_str());
    }
  }
  return saveToFile();
}

bool WifiCredentialStore::removeCredential(const std::string& ssid) {
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    const auto cred = find_if(credentials.begin(), credentials.end(),
                              [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
    if (cred == credentials.end()) {
      return false;  // Not found
    }
    credentials.erase(cred);
    LOG_DBG("WCS", "Removed credentials for: %s", ssid.c_str());
    if (ssid == lastConnectedSsid) lastConnectedSsid.clear();
  }
  return saveToFile();
}

std::optional<WifiCredential> WifiCredentialStore::findCredential(const std::string& ssid) const {
  std::lock_guard<std::mutex> lock(credentialMutex);

  const auto cred = find_if(credentials.begin(), credentials.end(),
                            [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });

  if (cred != credentials.end()) {
    return *cred;
  }

  return std::nullopt;
}

std::optional<WifiCredential> WifiCredentialStore::getCredentialAt(const size_t index) const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  if (index >= credentials.size()) return std::nullopt;
  return credentials[index];
}

std::optional<std::string> WifiCredentialStore::getSsidAt(const size_t index) const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  if (index >= credentials.size()) return std::nullopt;
  return credentials[index].ssid;
}

size_t WifiCredentialStore::getCredentialCount() const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  return credentials.size();
}

std::vector<WifiCredentialSummary> WifiCredentialStore::getCredentialSummaries() const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  std::vector<WifiCredentialSummary> summaries;
  summaries.reserve(credentials.size());
  for (const auto& credential : credentials) {
    summaries.push_back(
        {credential.ssid, !credential.password.empty(), credential.ssid == lastConnectedSsid, credential.enterprise});
  }
  return summaries;
}

bool WifiCredentialStore::hasSavedCredential(const std::string& ssid) const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  return find_if(credentials.begin(), credentials.end(),
                 [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; }) != credentials.end();
}

void WifiCredentialStore::setLastConnectedSsid(const std::string& ssid) {
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    if (lastConnectedSsid == ssid) return;
    lastConnectedSsid = ssid;
  }
  saveToFile();
}

std::string WifiCredentialStore::getLastConnectedSsid() const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  return lastConnectedSsid;
}

void WifiCredentialStore::clearLastConnectedSsid() {
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    if (lastConnectedSsid.empty()) return;
    lastConnectedSsid.clear();
  }
  saveToFile();
}

void WifiCredentialStore::clearAll() {
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    credentials.clear();
    lastConnectedSsid.clear();
  }
  saveToFile();
  LOG_DBG("WCS", "Cleared all WiFi credentials");
}
