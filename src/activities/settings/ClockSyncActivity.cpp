#include "ClockSyncActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ClockSyncActivity::onEnter() {
  Activity::onEnter();
  state = SYNCING;
  syncedTime[0] = '\0';

  if (WiFi.status() == WL_CONNECTED) {
    requestUpdate();
    return;
  }

  shouldTearDownWifiOnExit = true;
  launchWifiSelection();
}

void ClockSyncActivity::onExit() {
  Activity::onExit();

  if (shouldTearDownWifiOnExit && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void ClockSyncActivity::launchWifiSelection() {
  LOG_INF("CLK", "Manual sync requested without WiFi, launching WiFi selection");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ClockSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    LOG_INF("CLK", "WiFi selection cancelled before manual clock sync");
    finish();
    return;
  }

  state = SYNCING;
  requestUpdate();
}

void ClockSyncActivity::runSync() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("CLK", "Manual sync requested but WiFi is not connected after selection");
    state = NO_WIFI;
    requestUpdate();
    return;
  }

  const bool ok = halClock.syncFromNTP();
  if (!ok) {
    state = FAILED;
    requestUpdate();
    return;
  }

  // Mark as synced so the auto-sync hook stops firing on future WiFi connects.
  SETTINGS.clockHasBeenSynced = 1;
  SETTINGS.saveToFile();

  // Read the freshly synced time back for the user-facing confirmation.
  char buf[9];
  if (halClock.formatTime(buf, sizeof(buf), SETTINGS.clockFormat == 1)) {
    snprintf(syncedTime, sizeof(syncedTime), "%s", buf);
  }
  state = SUCCESS;
  requestUpdate();
}

void ClockSyncActivity::loop() {
  if (state == SYNCING) {
    // First-tick: render the "Syncing..." screen, then perform the (blocking) sync.
    // requestUpdateAndWait below forces the render before we block on WiFi.
    requestUpdateAndWait();
    runSync();
    return;
  }

  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void ClockSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLOCK_SYNC));

  const Rect body = UITheme::getContentBounds(renderer);
  const Rect title{body.x, body.y, body.width, body.height / 2 - 8};
  const Rect detail{body.x, body.y + body.height / 2 + 8, body.width, body.height / 2 - 8};

  switch (state) {
    case SYNCING:
      UITheme::drawCenteredWrappedText(renderer, body, UI_12_FONT_ID, tr(STR_CLOCK_SYNCING), 8);
      break;
    case SUCCESS: {
      UITheme::drawCenteredWrappedText(renderer, title, UI_12_FONT_ID, tr(STR_CLOCK_SYNC_OK), 4, true,
                                       EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::BOTTOM);
      if (syncedTime[0] != '\0') {
        // Keep translated labels separate from the bounded ASCII clock value.
        const int labelHeight = std::min(detail.height / 2, 2 * renderer.getLineHeight(UI_10_FONT_ID));
        UITheme::drawCenteredWrappedText(renderer, Rect{detail.x, detail.y, detail.width, labelHeight},
                                         UI_10_FONT_ID, tr(STR_CURRENT_TIME), 2, true,
                                         EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
        UITheme::drawCenteredWrappedText(renderer,
            Rect{detail.x, detail.y + labelHeight + 8, detail.width, detail.height - labelHeight - 8},
            UI_10_FONT_ID, syncedTime, 2, true, EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
      }
      break;
    }
    case NO_WIFI:
      UITheme::drawCenteredWrappedText(renderer, title, UI_12_FONT_ID, tr(STR_CLOCK_SYNC_NO_WIFI), 4, true,
                                       EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::BOTTOM);
      UITheme::drawCenteredWrappedText(renderer, detail, UI_10_FONT_ID, tr(STR_CLOCK_SYNC_NO_WIFI_HINT), 6, true,
                                       EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
      break;
    case FAILED:
      UITheme::drawCenteredWrappedText(renderer, title, UI_12_FONT_ID, tr(STR_CLOCK_SYNC_FAIL), 4, true,
                                       EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::BOTTOM);
      UITheme::drawCenteredWrappedText(renderer, detail, UI_10_FONT_ID, tr(STR_CHECK_SERIAL_OUTPUT), 6, true,
                                       EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
      break;
  }

  if (state != SYNCING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
