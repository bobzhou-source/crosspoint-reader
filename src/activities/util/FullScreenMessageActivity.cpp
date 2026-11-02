#include "FullScreenMessageActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "components/UITheme.h"

#include "fontIds.h"

void FullScreenMessageActivity::onEnter() {
  Activity::onEnter();

  Rect bounds = UITheme::getContentBounds(renderer, false);
  const int hintHeight = renderer.getLineHeight(UI_10_FONT_ID) * 3;
  const Rect hint{bounds.x, bounds.y + bounds.height - hintHeight, bounds.width, hintHeight};
  bounds.height -= hintHeight + 16;
  renderer.clearScreen();
  UITheme::drawCenteredWrappedText(renderer, bounds, UI_12_FONT_ID, text.c_str(), 8, true, style);
  UITheme::drawCenteredWrappedText(renderer, hint, UI_10_FONT_ID, tr(STR_SD_CARD_RESTART_HINT), 3);
  renderer.displayBuffer(refreshMode);
}
