#include "WifiDiagnostics.h"

#include <HalStorage.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace wifi_connection {
namespace {
struct Attempt {
  char ssid[33]{};
  uint8_t bssid[6]{};
  uint32_t started = 0;
  uint16_t reason = 0;
  int rssi = 0;
  int channel = 0;
  int authmode = -1;
  bool associated = false;
  bool gotIp = false;
  bool active = false;
};
Attempt attempt;
std::mutex diagnosticsMutex;
constexpr char LOG_PATH[] = "/.crosspoint/wifi-log.txt";
constexpr char PREVIOUS_LOG[] = "/.crosspoint/wifi-log.1.txt";

void event(arduino_event_id_t id, arduino_event_info_t info) {
  std::lock_guard<std::mutex> guard(diagnosticsMutex);
  if (!attempt.active) return;
  if (id == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    attempt.associated = true;
    attempt.reason = 0;
    attempt.channel = info.wifi_sta_connected.channel;
    attempt.authmode = info.wifi_sta_connected.authmode;
    memcpy(attempt.bssid, info.wifi_sta_connected.bssid, 6);
  } else if (id == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    attempt.reason = info.wifi_sta_disconnected.reason;
    attempt.rssi = info.wifi_sta_disconnected.rssi;
    if (memcmp(attempt.bssid, info.wifi_sta_disconnected.bssid, 6) != 0) {
      // A driver's retry may select another BSSID without ever associating.
      // Do not attribute the previous AP's channel/security to that rejection.
      attempt.channel = 0;
      attempt.authmode = -1;
    }
    memcpy(attempt.bssid, info.wifi_sta_disconnected.bssid, 6);
  } else if (id == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    attempt.gotIp = true;
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      attempt.rssi = ap.rssi;
      attempt.channel = ap.primary;
      attempt.authmode = ap.authmode;
      memcpy(attempt.bssid, ap.bssid, 6);
    }
  }
}
}  // namespace

void startDiagnostics(const std::string& ssid) {
  static bool registered = false;
  if (!registered) {
    WiFi.onEvent(event);
    registered = true;
  }
  std::lock_guard<std::mutex> guard(diagnosticsMutex);
  attempt = Attempt{};
  // Escape control characters so an SSID cannot forge additional log records.
  for (size_t i = 0; i < ssid.size() && i < 32; ++i) {
    const unsigned char c = ssid[i];
    attempt.ssid[i] = c < 32 || c == 127 || c == '"' ? '?' : c;
  }
  attempt.started = millis();
  attempt.active = true;
}

Failure lastFailure() {
  std::lock_guard<std::mutex> guard(diagnosticsMutex);
  return classifyFailure(attempt.reason, attempt.associated, attempt.gotIp);
}

void finishDiagnostics(const char* outcome) {
  Attempt snapshot;
  {
    std::lock_guard<std::mutex> guard(diagnosticsMutex);
    if (!attempt.active) return;
    attempt.active = false;
    snapshot = attempt;
  }
  wifi_ap_record_t ap{};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    snapshot.associated = true;
    snapshot.rssi = ap.rssi;
    snapshot.channel = ap.primary;
    snapshot.authmode = ap.authmode;
    memcpy(snapshot.bssid, ap.bssid, 6);
  }
  snapshot.gotIp = snapshot.gotIp || WiFi.status() == WL_CONNECTED;
  char line[224];
  const int count =
      snprintf(line, sizeof(line),
               "ms=%lu ssid=\"%s\" result=%s reason=%u rssi=%d channel=%d auth=%d bssid=%02x:%02x:%02x:%02x:%02x:%02x "
               "associated=%d dhcp_ip=%d duration_ms=%lu\n",
               static_cast<unsigned long>(snapshot.started), snapshot.ssid, outcome, snapshot.reason, snapshot.rssi,
               snapshot.channel, snapshot.authmode, snapshot.bssid[0], snapshot.bssid[1], snapshot.bssid[2],
               snapshot.bssid[3], snapshot.bssid[4], snapshot.bssid[5], snapshot.associated, snapshot.gotIp,
               static_cast<unsigned long>(millis() - snapshot.started));
  if (count <= 0 || count >= static_cast<int>(sizeof(line)) || !Storage.ensureDirectoryExists("/.crosspoint")) return;
  bool rotate = false;
  {
    auto file = Storage.open(LOG_PATH);
    rotate = file && file.size() + count > 8192;
  }
  if (rotate) {
    if (Storage.exists(PREVIOUS_LOG) && !Storage.remove(PREVIOUS_LOG)) return;
    if (!Storage.rename(LOG_PATH, PREVIOUS_LOG)) return;
  }
  auto file = Storage.open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (file) file.write(reinterpret_cast<const uint8_t*>(line), count);
}
}  // namespace wifi_connection
