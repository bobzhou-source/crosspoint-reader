#include "CrashActivity.h"

#include <GfxRenderer.h>
#include <HalSystem.h>
#include <I18n.h>

#include "components/UITheme.h"
#include "fontIds.h"

void CrashActivity::onEnter() {
  Activity::onEnter();

  panicMessage = HalSystem::getPanicInfo(false);
  if (panicMessage.empty()) {
    panicMessage = tr(STR_CRASH_NO_REASON);
  }

  requestUpdateAndWait();
}

void CrashActivity::loop() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void CrashActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  Rect body = UITheme::getContentBounds(renderer);
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int footerHeight = mappedInput.hasTouch() ? lineHeight * 2 + 16 : 0;
  const Rect footer{body.x, body.y + body.height - footerHeight, body.width, footerHeight};
  body.height -= footerHeight;

  // Crash report is a dead end, not a pushed screen: no back button.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CRASH_TITLE), nullptr,
                 false);

  const int descriptionHeight = body.height / 3;
  UITheme::drawCenteredWrappedText(renderer, Rect{body.x, body.y, body.width, descriptionHeight}, UI_10_FONT_ID,
                                   tr(STR_CRASH_DESCRIPTION), 10, true, EpdFontFamily::REGULAR,
                                   UITheme::TextVerticalAlignment::TOP);
  const int reasonY = body.y + descriptionHeight + 16;
  UITheme::drawCenteredWrappedText(renderer, Rect{body.x, reasonY, body.width, lineHeight * 2}, UI_10_FONT_ID,
                                   tr(STR_CRASH_REASON), 2, true, EpdFontFamily::BOLD);
  const int panicY = reasonY + lineHeight * 2 + 16;
  UITheme::drawCenteredWrappedText(renderer, Rect{body.x, panicY, body.width, body.y + body.height - panicY},
                                   UI_10_FONT_ID, panicMessage.c_str(), 12, true, EpdFontFamily::REGULAR,
                                   UITheme::TextVerticalAlignment::TOP);
  if (mappedInput.hasTouch()) {
    UITheme::drawCenteredWrappedText(renderer, footer, UI_10_FONT_ID, tr(STR_TAP_TO_RETURN), 2);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
