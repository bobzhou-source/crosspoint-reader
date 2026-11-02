#pragma once
#include "fontIds.h"

// FreeInkUI font slots. Row heights, header height, and touch sizes are not
// chosen here: FreeInkApp derives all metric tokens from the body font's line
// height (themeTokensForLineHeight). One fixed tier for every board — the
// user-facing UI-scale setting was removed.
struct UIScaleSpec {
  int smallFontId;
  int bodyFontId;
  int titleFontId;
};

inline UIScaleSpec uiScaleSpec() {
  UIScaleSpec spec{};
  spec.smallFontId = UI_10_FONT_ID;
  spec.bodyFontId = UI_12_FONT_ID;
  // Broad-coverage default. Individual page/book titles may opt into the
  // existing serif through BaseTheme::titleFontId after whole-string coverage
  // and render-mode checks; reader-font fallback remains independent.
  spec.titleFontId = UI_12_FONT_ID;
  return spec;
}
