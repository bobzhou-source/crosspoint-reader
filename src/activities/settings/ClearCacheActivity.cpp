#include "ClearCacheActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

void ClearCacheActivity::onEnter() {
  Activity::onEnter();

  state = WARNING;
  const char* options[] = {tr(STR_CANCEL), tr(STR_CLEAR_BUTTON)};
  const std::string warning = std::string(tr(STR_CLEAR_CACHE_WARNING_1)) + "\n" + tr(STR_CLEAR_CACHE_WARNING_2) +
                              "\n" + tr(STR_CLEAR_CACHE_WARNING_3) + " " + tr(STR_CLEAR_CACHE_WARNING_4);
  confirmPopup.show(tr(STR_CLEAR_READING_CACHE), warning.c_str(), options, 2, 0, [this](int idx) {
    if (idx == 1) {
      beginClear();
    } else {
      goBack();
    }
  });
  requestUpdate();
}

void ClearCacheActivity::onExit() { Activity::onExit(); }

void ClearCacheActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const Rect body = UITheme::getContentBounds(renderer);
  const Rect title{body.x, body.y, body.width, body.height / 2 - 8};
  const Rect detail{body.x, body.y + body.height / 2 + 8, body.width, body.height / 2 - 8};

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLEAR_READING_CACHE));

  if (state == WARNING) {
    // Keep the warning inside the modal; retain it if the modal is dismissed.

    if (confirmPopup.processRender(renderer, mappedInput)) return;
    const char* warnings[] = {tr(STR_CLEAR_CACHE_WARNING_1), tr(STR_CLEAR_CACHE_WARNING_2),
                              tr(STR_CLEAR_CACHE_WARNING_3), tr(STR_CLEAR_CACHE_WARNING_4)};
    const int slotHeight = body.height / 4;
    for (int i = 0; i < 4; ++i) {
      UITheme::drawCenteredWrappedText(renderer, Rect{body.x, body.y + i * slotHeight, body.width, slotHeight},
                                       UI_10_FONT_ID, warnings[i], 4, true,
                                       i == 1 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CLEAR_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CLEARING) {
    UITheme::drawCenteredWrappedText(renderer, body, UI_12_FONT_ID, tr(STR_CLEARING_CACHE), 8);
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    UITheme::drawCenteredWrappedText(renderer, title, UI_12_FONT_ID, tr(STR_CACHE_CLEARED), 4, true,
                                     EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::BOTTOM);
    std::string resultText = std::to_string(clearedCount) + " " + std::string(tr(STR_ITEMS_REMOVED));
    if (failedCount > 0) {
      resultText += ", " + std::to_string(failedCount) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    UITheme::drawCenteredWrappedText(renderer, detail, UI_10_FONT_ID, resultText.c_str(), 8, true,
                                     EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    UITheme::drawCenteredWrappedText(renderer, title, UI_12_FONT_ID, tr(STR_CLEAR_CACHE_FAILED), 4, true,
                                     EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::BOTTOM);
    UITheme::drawCenteredWrappedText(renderer, detail, UI_10_FONT_ID, tr(STR_CHECK_SERIAL_OUTPUT), 8, true,
                                     EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ClearCacheActivity::beginClear() {
  LOG_DBG("CLEAR_CACHE", "User confirmed, starting cache clear");
  {
    RenderLock lock(*this);
    state = CLEARING;
  }
  requestUpdateAndWait();
  clearCache();
}

void ClearCacheActivity::clearCache() {
  LOG_DBG("CLEAR_CACHE", "Clearing cache...");

  // Open .crosspoint directory
  auto root = Storage.open("/.crosspoint");
  if (!root || !root.isDirectory()) {
    LOG_DBG("CLEAR_CACHE", "Failed to open cache directory");
    if (root) root.close();
    state = FAILED;
    requestUpdate();
    return;
  }

  clearedCount = 0;
  failedCount = 0;
  char name[128];

  // Iterate through all entries in the directory
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    String itemName(name);

    // Only delete directories matching known book cache names.
    if (file.isDirectory() && isBookCacheDirectoryName(itemName.c_str())) {
      String fullPath = "/.crosspoint/" + itemName;
      LOG_DBG("CLEAR_CACHE", "Removing cache: %s", fullPath.c_str());

      file.close();  // Close before attempting to delete

      if (Storage.removeDir(fullPath.c_str())) {
        clearedCount++;
      } else {
        LOG_ERR("CLEAR_CACHE", "Failed to remove: %s", fullPath.c_str());
        failedCount++;
      }
    } else {
      file.close();
    }
  }
  root.close();

  LOG_DBG("CLEAR_CACHE", "Cache cleared: %d removed, %d failed", clearedCount, failedCount);

  state = SUCCESS;
  requestUpdate();
}

void ClearCacheActivity::loop() {
  if (state == WARNING) {
    if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      beginClear();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_DBG("CLEAR_CACHE", "User cancelled");
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
      goBack();
    }
    return;
  }
}
