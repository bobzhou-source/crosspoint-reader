#include "LocalsendActivity.h"

#include <FontCacheManager.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TaskWatchdog.h"

void LocalsendActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock(*this);
    state = State::Connecting;
    displayStatus = {};
    requestUpdate();
  }
  wifiStarted = true;
  if (WiFi.status() == WL_CONNECTED) {
    startReceiver();
    return;
  }
  auto picker = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!picker) {
    LOG_ERR("LSEND", "OOM: Wi-Fi selection activity");
    RenderLock lock(*this);
    state = State::MemoryError;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(picker), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      finish();
      return;
    }
    startReceiver();
  });
}

void LocalsendActivity::startReceiver() {
  RenderLock lock(*this);
  // Allocate the bounded transport once for this activity, not on the main
  // task's small stack. Drop rebuildable SD font caches before Wi-Fi's peak.
  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
  server = makeUniqueNoThrow<LocalsendServer>();
  if (!server) {
    LOG_ERR("LSEND", "OOM: receiver (%u bytes)", static_cast<unsigned>(sizeof(LocalsendServer)));
    state = State::MemoryError;
    requestUpdate();
    return;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto lines = renderer.wrappedText(SMALL_FONT_ID, tr(STR_LOCALSEND_HINT),
                                          renderer.getScreenWidth() - metrics.contentSidePadding * 2, 2);
  instructionCount = lines.size();
  for (size_t i = 0; i < instructionCount; i++) {
    snprintf(instructions[i], sizeof(instructions[i]), "%s", lines[i].c_str());
  }
  snprintf(connectedSSID, sizeof(connectedSSID), "%s", WiFi.SSID().c_str());
  snprintf(connectedIP, sizeof(connectedIP), "%s:%u", WiFi.localIP().toString().c_str(), LocalsendServer::PORT);
  state = server->begin() ? State::Listening : State::Failed;
  displayStatus = server->getStatus();
  requestUpdate();
}

void LocalsendActivity::onExit() {
  if (server) {
    server->stop();
    server.reset();
  }
  Activity::onExit();
  // Match the other transfer activities: a clean reboot releases the SDK's
  // retained Wi-Fi allocations before returning to memory-sensitive reading.
  if (wifiStarted && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void LocalsendActivity::refreshStatus(bool force) {
  if (!server) return;
  const auto& status = server->getStatus();
  const uint32_t now = millis();
  const bool transition = status.revision != displayStatus.revision || status.receiving != displayStatus.receiving;
  const bool progress = status.received != displayStatus.received || status.total != displayStatus.total;
  if (!force && !transition && (!progress || now - lastProgressRender < 1500)) return;
  RenderLock lock(*this);
  displayStatus = status;
  state = server->isRunning() ? State::Listening : State::Failed;
  lastProgressRender = now;
  requestUpdate();
}

void LocalsendActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (!server || !server->isRunning()) return;
  resetTaskWatchdogIfSubscribed();
  server->poll();
  refreshStatus();
  // poll() performs bounded slices; input remains owned by the main loop.
  yield();
}

void LocalsendActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int margin = metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_LOCALSEND));
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (state == State::Connecting || state == State::MemoryError) {
    renderer.drawCenteredText(UI_12_FONT_ID, height / 2,
                              state == State::Connecting ? tr(STR_CONNECTING) : tr(STR_MEMORY_ERROR));
    renderer.displayBuffer();
    return;
  }
  if (state == State::Failed) {
    const char* message = displayStatus.error == LocalsendServer::Error::FileStorage ? tr(STR_LOCALSEND_STORAGE_FAILED)
                                                                                  : tr(STR_CONNECTION_FAILED);
    renderer.drawCenteredText(UI_12_FONT_ID, height / 2, message);
    renderer.displayBuffer();
    return;
  }

  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, width, metrics.tabBarHeight},
                    connectedSSID, connectedIP);
  int y = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 2;
  // Instruction wrapping is cached at startup; rendering does not allocate.
  // Clip filenames to the content bounds in every orientation.
  const auto previousClip = renderer.getClipRect();
  renderer.setClipRect(margin, y, width - margin * 2, height - y - metrics.buttonHintsHeight);
  for (size_t i = 0; i < instructionCount; i++) {
    renderer.drawText(SMALL_FONT_ID, margin, y, instructions[i]);
    y += lineHeight + metrics.verticalSpacing;
  }
  renderer.drawText(SMALL_FONT_ID, margin, y, tr(STR_LOCALSEND_DESTINATION));
  y += lineHeight + metrics.verticalSpacing * 3;

  const char* message = tr(STR_LOCALSEND_WAITING);
  switch (displayStatus.error) {
    case LocalsendServer::Error::None:
      if (displayStatus.receiving) message = tr(STR_CALIBRE_RECEIVING);
      else if (displayStatus.completedFiles) message = tr(STR_CALIBRE_RECEIVED);
      break;
    case LocalsendServer::Error::Network:
      message = tr(STR_CONNECTION_FAILED);
      break;
    case LocalsendServer::Error::FileStorage:
      message = tr(STR_LOCALSEND_STORAGE_FAILED);
      break;
    case LocalsendServer::Error::Transfer:
      message = tr(STR_LOCALSEND_TRANSFER_FAILED);
      break;
    case LocalsendServer::Error::Cancelled:
      message = tr(STR_LOCALSEND_CANCELLED);
      break;
  }
  renderer.drawText(UI_12_FONT_ID, margin, y, message);
  y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
  renderer.drawText(SMALL_FONT_ID, margin, y, displayStatus.fileName);
  y += lineHeight + metrics.verticalSpacing;
  if (displayStatus.receiving) {
    // Pass bounded percentage counters to the shared progress-bar renderer.
    const int percent = displayStatus.total
                            ? static_cast<int>(static_cast<double>(displayStatus.received) * 100.0 /
                                               static_cast<double>(displayStatus.total))
                            : 0;
    GUI.drawProgressBar(renderer, Rect{margin, y, width - margin * 2, metrics.progressBarHeight}, percent, 100);
  }
  renderer.setClipRect(previousClip[0], previousClip[1], previousClip[2], previousClip[3]);
  renderer.displayBuffer();
}
