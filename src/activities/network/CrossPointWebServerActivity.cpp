#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstddef>

#include "MappedInputManager.h"
#include "NetworkModeSelectionActivity.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "activities/network/CalibreConnectActivity.h"
#include "activities/network/LocalsendActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"
#include "util/TaskWatchdog.h"

namespace {
// AP Mode configuration
constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr const char* AP_PASSWORD = nullptr;  // Open network for ease of use
constexpr const char* AP_HOSTNAME = "crosspoint";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr int QR_CODE_WIDTH = 198;

// DNS server for captive portal (redirects all DNS queries to our IP)
DNSServer* dnsServer = nullptr;
constexpr uint16_t DNS_PORT = 53;

void stopDnsServer() {
  if (!dnsServer) return;

  dnsServer->stop();
  delete dnsServer;
  dnsServer = nullptr;
}

void restartMdns(const char* hostname, const char* tag) {
  MDNS.end();
  if (MDNS.begin(hostname)) {
    LOG_DBG(tag, "mDNS started: http://%s.local/", hostname);
  } else {
    LOG_DBG(tag, "WARNING: mDNS failed to start");
  }
}

// 0..4 bars from RSSI (dBm), with 3 dBm hysteresis on currentBars to suppress flicker.
int barsForRssi(int rssi, int currentBars) {
  static constexpr int RISE_DBM[] = {-85, -75, -65, -55};
  static constexpr int FALL_DBM[] = {-88, -78, -68, -58};
  int bars = std::clamp(currentBars, 0, 4);
  while (bars < 4 && rssi >= RISE_DBM[bars]) bars++;
  while (bars > 0 && rssi < FALL_DBM[bars - 1]) bars--;
  return bars;
}
}  // namespace

void CrossPointWebServerActivity::onEnter() {
  Activity::onEnter();

  LOG_DBG("WEBACT", "Free heap at onEnter: %d bytes", ESP.getFreeHeap());

  // Heap-critical transition: WiFi (~45KB) plus the web server have to fit in
  // what's left of the ~380KB parts. SD-font caches retained for the CJK UI
  // fallback (mini glyph/kern arenas, kern class tables) are rebuildable on
  // demand — release them up front instead of aborting in startWebServer()
  // when the heap comes up short (observed on X3 with a Korean SD font).
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
    LOG_DBG("WEBACT", "Free heap after SD font cache release: %d bytes", ESP.getFreeHeap());
  }

  // Reset state
  state = WebServerActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  requestUpdate();

  // Launch network mode selection subactivity
  LOG_DBG("WEBACT", "Launching NetworkModeSelectionActivity...");
  startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             onGoHome();
                           } else {
                             onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                           }
                         });
}

void CrossPointWebServerActivity::onExit() {
  Activity::onExit();

  LOG_DBG("WEBACT", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  state = WebServerActivityState::SHUTTING_DOWN;
  stopDnsServer();
  MDNS.end();

  // Skip reboot if WiFi was never activated (e.g. user backed out of mode selection).
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    if (isApMode) {
      WiFi.softAPdisconnect(true);
    } else {
      WiFi.disconnect(false);
    }
    delay(30);
    silentRestart();
  }

  LOG_DBG("WEBACT", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServerActivity::onNetworkModeSelected(const NetworkMode mode) {
  const char* modeName = "Join Network";
  if (mode == NetworkMode::CONNECT_CALIBRE) {
    modeName = "Connect to Calibre";
  } else if (mode == NetworkMode::CREATE_HOTSPOT) {
    modeName = "Create Hotspot";
#if FREEINK_CAP_USB_MSC
  } else if (mode == NetworkMode::USB_DRIVE) {
    modeName = "USB Drive";
#endif
  }
  LOG_DBG("WEBACT", "Network mode selected: %s", modeName);

#if FREEINK_CAP_USB_MSC
  if (mode == NetworkMode::USB_DRIVE) {
    activityManager.goToUsbDrive();
    return;
  }
#endif

  networkMode = mode;
  isApMode = (mode == NetworkMode::CREATE_HOTSPOT);

  if (mode == NetworkMode::CONNECT_CALIBRE || mode == NetworkMode::LOCALSEND) {
    std::unique_ptr<Activity> transferActivity;
    if (mode == NetworkMode::LOCALSEND) {
      transferActivity = std::make_unique<LocalsendActivity>(renderer, mappedInput);
    } else {
      transferActivity = std::make_unique<CalibreConnectActivity>(renderer, mappedInput);
    }
    startActivityForResult(
        std::move(transferActivity), [this](const ActivityResult& result) {
          state = WebServerActivityState::MODE_SELECTION;

          startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                                 [this](const ActivityResult& result) {
                                   if (result.isCancelled) {
                                     onGoHome();
                                   } else {
                                     onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                                   }
                                 });
        });
    return;
  }

  if (mode == NetworkMode::JOIN_NETWORK) {
    // STA mode - launch WiFi selection
    LOG_DBG("WEBACT", "Turning on WiFi (STA mode)...");
    WiFi.mode(WIFI_STA);

    state = WebServerActivityState::WIFI_SELECTION;
    LOG_DBG("WEBACT", "Launching WifiSelectionActivity...");
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& wifi = std::get<WifiResult>(result.data);
                               connectedIP = wifi.ip;
                               connectedSSID = wifi.ssid;
                             }
                             onWifiSelectionComplete(!result.isCancelled);
                           });
  } else {
    // AP mode - start access point
    state = WebServerActivityState::AP_STARTING;
    requestUpdate();
    startAccessPoint();
  }
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  LOG_DBG("WEBACT", "WifiSelectionActivity completed, connected=%d", connected);

  if (connected) {
    // Get connection info before exiting subactivity
    isApMode = false;

    // Start mDNS for hostname resolution
    restartMdns(AP_HOSTNAME, "WEBACT");

    // Start the web server
    startWebServer();
  } else {
    // User cancelled - go back to mode selection
    state = WebServerActivityState::MODE_SELECTION;

    startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               onGoHome();
                             } else {
                               onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                             }
                           });
  }
}

void CrossPointWebServerActivity::startAccessPoint() {
  LOG_DBG("WEBACT", "Starting Access Point mode...");
  LOG_DBG("WEBACT", "Free heap before AP start: %d bytes", ESP.getFreeHeap());

  // Configure and start the AP
  WiFi.mode(WIFI_AP);
  delay(100);

  // Start soft AP
  bool apStarted;
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  } else {
    // Open network (no password)
    apStarted = WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  }

  if (!apStarted) {
    LOG_ERR("WEBACT", "ERROR: Failed to start Access Point!");
    onGoHome();
    return;
  }

  delay(100);  // Wait for AP to fully initialize

  // Get AP IP address
  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = AP_SSID;

  LOG_DBG("WEBACT", "Access Point started!");
  LOG_DBG("WEBACT", "SSID: %s", AP_SSID);
  LOG_DBG("WEBACT", "IP: %s", connectedIP.c_str());

  // Start mDNS for hostname resolution
  restartMdns(AP_HOSTNAME, "WEBACT");

  // Start DNS server for captive portal behavior
  // This redirects all DNS queries to our IP, making any domain typed resolve to us
  stopDnsServer();
  dnsServer = new DNSServer();
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(DNS_PORT, "*", apIP);
  LOG_DBG("WEBACT", "DNS server started for captive portal");

  LOG_DBG("WEBACT", "Free heap after AP start: %d bytes", ESP.getFreeHeap());

  // Start the web server
  startWebServer();
}

void CrossPointWebServerActivity::startWebServer() {
  LOG_DBG("WEBACT", "Starting web server...");

  // Repeat the release right before the allocation: the WiFi selection screen
  // rendered since onEnter(), and a CJK SSID repopulates the SD-font caches.
  if (auto* fcm = renderer.getFontCacheManager()) {
    LOG_DBG("WEBACT", "Free heap before SD font cache release: %d bytes", ESP.getFreeHeap());
    fcm->releaseSdFontCaches();
    LOG_DBG("WEBACT", "Free heap before server alloc: %d bytes", ESP.getFreeHeap());
  }

  // Create the web server instance
  webServer.reset(new CrossPointWebServer());
  webServer->begin();

  if (webServer->isRunning()) {
    state = WebServerActivityState::SERVER_RUNNING;
    LOG_DBG("WEBACT", "Web server started successfully");
    lastWifiBars = isApMode ? 0 : barsForRssi(WiFi.RSSI(), 0);

    // Force an immediate render since we're transitioning from a subactivity
    // that had its own rendering task. We need to make sure our display is shown.
    requestUpdate();
  } else {
    LOG_ERR("WEBACT", "ERROR: Failed to start web server!");
    webServer.reset();
    // Go back on error
    onGoHome();
  }
}

void CrossPointWebServerActivity::loop() {
  // Handle different states
  if (state == WebServerActivityState::SERVER_RUNNING) {
    // Handle DNS requests for captive portal (AP mode only)
    if (isApMode && dnsServer) {
      dnsServer->processNextRequest();
    }

    // STA mode: Monitor WiFi connection health
    if (!isApMode && webServer && webServer->isRunning()) {
      static unsigned long lastWifiCheck = 0;
      if (millis() - lastWifiCheck > 2000) {  // Check every 2 seconds
        lastWifiCheck = millis();
        const wl_status_t wifiStatus = WiFi.status();
        // Driver auto-reconnect handles retries; abandon (via onGoHome) only
        // after WIFI_ABANDON_MS, otherwise the activity freezes on a blip.
        bool repaint = false;
        if (wifiStatus != WL_CONNECTED) {
          if (consecutiveDisconnects == 0) {
            firstDisconnectAt = millis();
            repaint = true;
          }
          consecutiveDisconnects++;
          LOG_DBG("WEBACT", "WiFi not connected (status=%d, consecutive=%d, total=%lu ms)", wifiStatus,
                  consecutiveDisconnects, millis() - firstDisconnectAt);
          if (millis() - firstDisconnectAt > WIFI_ABANDON_MS) {
            LOG_DBG("WEBACT", "WiFi unavailable for >%lu s; returning to network selection", WIFI_ABANDON_MS / 1000UL);
            state = WebServerActivityState::SHUTTING_DOWN;
            onGoHome();
            return;
          }
        } else {
          if (consecutiveDisconnects > 0) {
            LOG_DBG("WEBACT", "WiFi recovered after %d failed checks (%lu ms)", consecutiveDisconnects,
                    millis() - firstDisconnectAt);
            repaint = true;
          }
          consecutiveDisconnects = 0;
          firstDisconnectAt = 0;
          const int rssi = WiFi.RSSI();
          if (rssi < -75) {
            LOG_DBG("WEBACT", "Warning: Weak WiFi signal: %d dBm", rssi);
          }
          const int bars = barsForRssi(rssi, lastWifiBars);
          if (bars != lastWifiBars) {
            lastWifiBars = bars;
            repaint = true;
          }
        }
        if (repaint) requestUpdate();
      }
    }

    // Handle web server requests - maximize throughput with watchdog safety
    if (webServer && webServer->isRunning()) {
      const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;

      // Log if there's a significant gap between handleClient calls (>100ms)
      if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
        LOG_DBG("WEBACT", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
      }

      // Reset watchdog BEFORE processing - HTTP header parsing can be slow
      resetTaskWatchdogIfSubscribed();

      // Process HTTP requests in tight loop for maximum throughput
      // More iterations = more data processed per main loop cycle
      constexpr int MAX_ITERATIONS = 500;
      for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
        webServer->handleClient();
        // Reset watchdog every 32 iterations
        if ((i & 0x1F) == 0x1F) {
          resetTaskWatchdogIfSubscribed();
        }
        // Yield and check for exit button every 64 iterations
        if ((i & 0x3F) == 0x3F) {
          yield();
          // Pump input inside this blocking loop so exit events remain responsive.
          mappedInput.update(true);
          // Home remains available now; other configured actions are deferred
          // to the next main-loop pass.
          if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
            onGoHome();
            return;
          }
        }
      }
      lastHandleClientTime = millis();
    }

    // Also check outside the request-processing loop.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
      onGoHome();
      return;
    }
  }
}

void CrossPointWebServerActivity::render(RenderLock&&) {
  // Only render our own UI when server is running
  // Subactivities handle their own rendering
  if (state == WebServerActivityState::SERVER_RUNNING || state == WebServerActivityState::AP_STARTING) {
    renderer.clearScreen();
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();

    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER), nullptr);

    if (state == WebServerActivityState::SERVER_RUNNING) {
      GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                        connectedSSID.c_str());
      renderServerRunning();
    } else {
      UITheme::drawCenteredWrappedText(renderer, UITheme::getContentBounds(renderer), UI_12_FONT_ID,
                                       tr(STR_STARTING_HOTSPOT), 8);
    }
    renderer.displayBuffer();
  }
}

void CrossPointWebServerActivity::renderServerRunning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER), nullptr);
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    connectedSSID.c_str());

  if (!isApMode) {
    renderWifiIndicator(metrics.topPadding + metrics.headerHeight);
  }

  Rect body = UITheme::getContentBounds(renderer);
  body.y += metrics.tabBarHeight;
  body.height = std::max(0, body.height - metrics.tabBarHeight);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto drawConnectionPanel = [&](const Rect panel, const char* heading, const std::string& qrText,
                                        const char* primary, const char* secondary) {
    const int headingHeight = lineHeight * 2;
    const int detailsHeight = lineHeight * (secondary ? 4 : 2);
    const int qrSize = std::max(0, std::min({QR_CODE_WIDTH, panel.width, panel.height - headingHeight - detailsHeight - 24}));
    UITheme::drawCenteredWrappedText(renderer, Rect{panel.x, panel.y, panel.width, headingHeight},
                                     UI_10_FONT_ID, heading, 2, true, EpdFontFamily::BOLD);
    const int qrY = panel.y + headingHeight + 8;
    if (qrSize > 0) {
      QrUtils::drawQrCode(renderer, Rect{panel.x + (panel.width - qrSize) / 2, qrY, qrSize, qrSize}, qrText);
    }
    const int textY = qrY + qrSize + 8;
    UITheme::drawCenteredWrappedText(renderer, Rect{panel.x, textY, panel.width, lineHeight * 2},
                                     UI_10_FONT_ID, primary, 2);
    if (secondary) {
      UITheme::drawCenteredWrappedText(
          renderer, Rect{panel.x, textY + lineHeight * 2 + 8, panel.width, lineHeight * 2},
          SMALL_FONT_ID, secondary, 2);
    }
  };
  if (isApMode) {
    Rect network = body;
    Rect address = body;
    if (body.width > body.height) {
      network.width = (body.width - 24) / 2;
      address.x = network.x + network.width + 24;
      address.width = network.width;
    } else {
      network.height = (body.height - 24) / 2;
      address.y = network.y + network.height + 24;
      address.height = network.height;
    }
    const std::string wifiConfig = std::string("WIFI:T:nopass;S:") + connectedSSID + ";;";
    drawConnectionPanel(network, tr(STR_CONNECT_WIFI_HINT), wifiConfig, connectedSSID.c_str(), nullptr);
    const std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
    const std::string ipUrl = tr(STR_OR_HTTP_PREFIX) + connectedIP + "/";
    drawConnectionPanel(address, tr(STR_OPEN_URL_HINT), hostnameUrl, hostnameUrl.c_str(), ipUrl.c_str());
  } else {
    UITheme::drawCenteredWrappedText(renderer, Rect{body.x, body.y, body.width, lineHeight * 2},
                                     UI_10_FONT_ID, tr(STR_SCAN_QR_HINT), 2);
    body.y += lineHeight * 2 + 8;
    body.height -= lineHeight * 2 + 8;
    const std::string webInfo = "http://" + connectedIP + "/";
    const std::string hostnameUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + AP_HOSTNAME + ".local/";
    drawConnectionPanel(body, tr(STR_OPEN_URL_HINT), webInfo, webInfo.c_str(), hostnameUrl.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CrossPointWebServerActivity::renderWifiIndicator(int subHeaderTop) const {
  constexpr int BAR_COUNT = 4;
  constexpr int BAR_WIDTH = 4;
  constexpr int BAR_GAP = 2;
  constexpr int ICON_HEIGHT = 14;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int iconWidth = BAR_COUNT * BAR_WIDTH + (BAR_COUNT - 1) * BAR_GAP;
  const int iconRight = renderer.getScreenWidth() - metrics.contentSidePadding;
  const int iconLeft = iconRight - iconWidth;
  const int iconBottom = subHeaderTop + metrics.tabBarHeight - metrics.verticalSpacing;

  const bool wifiUp = (WiFi.status() == WL_CONNECTED) && (consecutiveDisconnects == 0);
  if (wifiUp) {
    for (int i = 0; i < BAR_COUNT; i++) {
      const int barHeight = (i + 1) * ICON_HEIGHT / BAR_COUNT;
      const int x = iconLeft + i * (BAR_WIDTH + BAR_GAP);
      const int y = iconBottom - barHeight;
      if (i < lastWifiBars) {
        renderer.fillRect(x, y, BAR_WIDTH, barHeight, true);
      } else {
        renderer.drawRect(x, y, BAR_WIDTH, barHeight, true);
      }
    }
  } else {
    const int xSize = ICON_HEIGHT;
    const int x0 = iconRight - xSize;
    const int y0 = iconBottom - xSize;
    renderer.drawLine(x0, y0, x0 + xSize, y0 + xSize, 2, true);
    renderer.drawLine(x0, y0 + xSize, x0 + xSize, y0, 2, true);
  }
}
