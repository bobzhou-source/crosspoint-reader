#include "CalibreConnectActivity.h"

#include <ESPmDNS.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TaskWatchdog.h"

namespace {
constexpr const char* HOSTNAME = "crosspoint";
}  // namespace

void CalibreConnectActivity::onEnter() {
  Activity::onEnter();

  requestUpdate();
  state = CalibreConnectState::WIFI_SELECTION;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  lastProgressReceived = 0;
  lastProgressTotal = 0;
  currentUploadName.clear();
  lastCompleteName.clear();
  lastCompleteAt = 0;
  lastProcessedCompleteAt = 0;
  exitRequested = false;

  if (WiFi.status() != WL_CONNECTED) {
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
    connectedIP = WiFi.localIP().toString().c_str();
    connectedSSID = WiFi.SSID().c_str();
    startWebServer();
  }
}

void CalibreConnectActivity::onExit() {
  Activity::onExit();

  MDNS.end();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void CalibreConnectActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    finish();
    return;
  }

  startWebServer();
}

void CalibreConnectActivity::startWebServer() {
  state = CalibreConnectState::SERVER_STARTING;
  requestUpdate();

  MDNS.end();
  if (MDNS.begin(HOSTNAME)) {
    // mDNS is optional for the Calibre plugin but still helpful for users.
    LOG_DBG("CAL", "mDNS started: http://%s.local/", HOSTNAME);
  }

  // Heap-critical allocation: SD-font caches retained for the CJK UI fallback
  // are rebuildable — release them (again: the WiFi selection screen may have
  // repopulated them rendering a CJK SSID) so the server object doesn't abort
  // on OOM. See CrossPointWebServerActivity::startWebServer().
  if (auto* fcm = renderer.getFontCacheManager()) {
    LOG_DBG("CAL", "Free heap before SD font cache release: %d bytes", ESP.getFreeHeap());
    fcm->releaseSdFontCaches();
    LOG_DBG("CAL", "Free heap before server alloc: %d bytes", ESP.getFreeHeap());
  }

  webServer.reset(new CrossPointWebServer());
  webServer->begin();

  if (webServer->isRunning()) {
    state = CalibreConnectState::SERVER_RUNNING;
    requestUpdate();
  } else {
    state = CalibreConnectState::ERROR;
    requestUpdate();
  }
}

void CalibreConnectActivity::stopWebServer() {
  if (webServer) {
    webServer->stop();
    webServer.reset();
  }
}

void CalibreConnectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitRequested = true;
  }

  if (webServer && webServer->isRunning()) {
    const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;
    if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
      LOG_DBG("CAL", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
    }

    resetTaskWatchdogIfSubscribed();
    constexpr int MAX_ITERATIONS = 80;
    for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
      webServer->handleClient();
      if ((i & 0x07) == 0x07) {
        resetTaskWatchdogIfSubscribed();
      }
      if ((i & 0x0F) == 0x0F) {
        yield();
        if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
          exitRequested = true;
          break;
        }
      }
    }
    lastHandleClientTime = millis();

    const auto status = webServer->getWsUploadStatus();
    bool changed = false;
    if (status.inProgress) {
      if (status.received != lastProgressReceived || status.total != lastProgressTotal ||
          status.filename != currentUploadName) {
        lastProgressReceived = status.received;
        lastProgressTotal = status.total;
        currentUploadName = status.filename;
        changed = true;
      }
    } else if (lastProgressReceived != 0 || lastProgressTotal != 0) {
      lastProgressReceived = 0;
      lastProgressTotal = 0;
      currentUploadName.clear();
      changed = true;
    }
    // Only update lastCompleteAt if the server has a NEW value (not one we already processed)
    // This prevents restoring an old value after the 6s timeout clears it
    if (status.lastCompleteAt != 0 && status.lastCompleteAt != lastProcessedCompleteAt) {
      lastCompleteAt = status.lastCompleteAt;
      lastCompleteName = status.lastCompleteName;
      lastProcessedCompleteAt = status.lastCompleteAt;  // Mark this value as processed
      changed = true;
    }
    if (lastCompleteAt > 0 && (millis() - lastCompleteAt) >= 6000) {
      lastCompleteAt = 0;
      lastCompleteName.clear();
      // Note: we DON'T reset lastProcessedCompleteAt here, so we won't re-process the old server value
      changed = true;
    }
    if (changed) {
      requestUpdate();
    }
  }

  if (exitRequested) {
    finish();
    return;
  }
}

void CalibreConnectActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CALIBRE_WIRELESS));
  const Rect body = UITheme::getContentBounds(renderer);

  if (state == CalibreConnectState::SERVER_STARTING) {
    UITheme::drawCenteredWrappedText(renderer, body, UI_12_FONT_ID, tr(STR_CALIBRE_STARTING), 6);
  } else if (state == CalibreConnectState::ERROR) {
    UITheme::drawCenteredWrappedText(renderer, body, UI_12_FONT_ID, tr(STR_CONNECTION_FAILED), 6, true,
                                     EpdFontFamily::BOLD);
  } else if (state == CalibreConnectState::SERVER_RUNNING) {
    GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                      connectedSSID.c_str(), (std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP).c_str());

    Rect setup = body;
    setup.y += metrics.tabBarHeight;
    setup.height = std::max(0, setup.height - metrics.tabBarHeight);
    Rect status = setup;
    if (setup.width > setup.height) {
      setup.width = (setup.width - 24) / 2;
      status.x = setup.x + setup.width + 24;
      status.width = setup.width;
    } else {
      setup.height = (setup.height - 24) * 2 / 3;
      status.y = setup.y + setup.height + 24;
      status.height = body.y + body.height - status.y;
    }
    const int headingHeight = renderer.getLineHeight(UI_12_FONT_ID) * 2;
    UITheme::drawCenteredWrappedText(renderer, Rect{setup.x, setup.y, setup.width, headingHeight},
                                     UI_12_FONT_ID, tr(STR_CALIBRE_SETUP), 2, true, EpdFontFamily::BOLD);
    const char* instructions[] = {tr(STR_CALIBRE_INSTRUCTION_1), tr(STR_CALIBRE_INSTRUCTION_2),
                                   tr(STR_CALIBRE_INSTRUCTION_3), tr(STR_CALIBRE_INSTRUCTION_4)};
    const int slotHeight = std::max(0, (setup.height - headingHeight - 8) / 4);
    for (int i = 0; i < 4; ++i) {
      UITheme::drawCenteredWrappedText(renderer,
          Rect{setup.x, setup.y + headingHeight + 8 + i * slotHeight, setup.width, slotHeight},
          SMALL_FONT_ID, instructions[i], 4);
    }
    UITheme::drawCenteredWrappedText(renderer, Rect{status.x, status.y, status.width, headingHeight},
                                     UI_12_FONT_ID, tr(STR_CALIBRE_STATUS), 2, true, EpdFontFamily::BOLD);
    int y = status.y + headingHeight + 8;
    const int endY = status.y + status.height;
    if (lastProgressTotal > 0 && lastProgressReceived <= lastProgressTotal) {
      std::string label = tr(STR_CALIBRE_RECEIVING);
      if (!currentUploadName.empty()) label += ": " + currentUploadName;
      const int labelHeight = std::min(renderer.getLineHeight(SMALL_FONT_ID) * 2,
                                       std::max(0, endY - y - metrics.progressBarHeight - 16));
      UITheme::drawCenteredWrappedText(renderer, Rect{status.x, y, status.width, labelHeight},
                                       SMALL_FONT_ID, label.c_str(), 2);
      y += labelHeight + 8;
      GUI.drawProgressBar(renderer, Rect{status.x, y, status.width, metrics.progressBarHeight},
                          lastProgressReceived, lastProgressTotal);
      y += metrics.progressBarHeight + 8;
    }
    if (lastCompleteAt > 0 && (millis() - lastCompleteAt) < 6000) {
      const std::string message = std::string(tr(STR_CALIBRE_RECEIVED)) + lastCompleteName;
      UITheme::drawCenteredWrappedText(renderer, Rect{status.x, y, status.width, std::max(0, endY - y)},
                                       SMALL_FONT_ID, message.c_str(), 6);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
