#pragma once
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/UiAppHost.h"

// Shared slider-dialog popup, drawn over the current screen (no clear) via
// fui::optionDialog with the same flat chrome OptionPopup uses: title caption,
// a prominent value readout, a full-width slider with its range endpoints,
// and a [-] [+] [Confirm] row on touch boards or two step-hint lines on
// button boards. Cancel is Back or a tap outside the dialog.
// Used by the interval and percent selection
// dialogs, which differ only in how they format the readout, endpoints, and
// hints and what the actions do.
struct UiSliderDialogSpec {
  const char* title = nullptr;    // dialog caption
  const char* readout = nullptr;  // preformatted current-value text
  int value = 0;                  // slider position (0-based within max)
  int max = 1;                    // slider range
  // Range endpoints, drawn small over the slider band's ends ("0%" / "100%").
  const char* minLabel = nullptr;
  const char* maxLabel = nullptr;
  freeink::ui::ActionId sliderAction = 0;
  freeink::ui::ActionId stepAction = 0;  // dispatched with value -1 / +1
  freeink::ui::ActionId okAction = 0;
  // Absorbs taps on the dialog body (needs no handler); the activity treats
  // an unrouted release with real coordinates as tap-outside = cancel.
  freeink::ui::ActionId chromeAction = 0;
  // Step hints for button boards (small step, large step); skipped on touch.
  const char* hintLine1 = nullptr;
  const char* hintLine2 = nullptr;
};

inline void buildSliderDialogScreen(UiAppHost::UiScreen& screen, freeink::ui::GfxRendererTarget& uiTarget,
                                    const MappedInputManager& mappedInput, const UiSliderDialogSpec& spec) {
  namespace fui = freeink::ui;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();

  const bool touch = mappedInput.hasTouch();
  const int16_t bandHeight = touch ? 48 : 24;
  const int16_t capLh = uiTarget.lineHeight(theme.smallText.font);
  const int16_t sliderH = static_cast<int16_t>(capLh + theme.spaceMd + bandHeight);
  const int16_t hintsH = touch ? 0 : static_cast<int16_t>(capLh * 2 + theme.spaceSm + theme.spaceMd);

  // Reuse the theme's popup and button styles. Props contain two StyleSets,
  // so keep them off the constrained render-task stack.
  fui::DialogOption options[3];
  options[0].label = "-";
  options[0].action = spec.stepAction;
  options[0].value = -1;
  options[1].label = "+";
  options[1].action = spec.stepAction;
  options[1].value = +1;
  options[2].label = tr(STR_CONFIRM);
  options[2].action = spec.okAction;
  options[2].state = fui::StateSelected;

  static fui::OptionDialogProps props;
  props.title = spec.title;
  props.headline = spec.readout;
  props.options = touch ? options : nullptr;
  props.optionCount = touch ? 3 : 0;
  props.inputMask = fui::InputTouch;
  props.titleText = theme.smallText;
  props.titleText.bold = false;
  props.titleText.align = UITheme::isRtl() ? fui::TextAlign::Right : fui::TextAlign::Left;
  props.titleText.maxLines = 2;
  props.headlineText = theme.titleText;
  props.headlineText.bold = false;
  props.headlineText.align = props.titleText.align;
  props.headlineText.maxLines = 2;
  props.buttonText = theme.bodyText;
  props.buttonText.maxLines = 2;
  props.buttonStyles = theme.button;
  const int16_t innerPadding = static_cast<int16_t>(metrics.optionPopupInnerPadding);
  props.padding = fui::Insets{innerPadding, innerPadding, innerPadding, innerPadding};
  props.gap = static_cast<int16_t>(metrics.optionPopupItemSpacing);
  props.styles = theme.popup;
  props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
  props.styles.normal.borderWidth = static_cast<uint8_t>(metrics.popupFrameThickness);
  props.styles.normal.radius = static_cast<uint8_t>(metrics.popupCornerRadius);
  props.styles.selected = props.styles.normal;
  props.styles.focused = props.styles.normal;
  props.styles.active = props.styles.normal;
  props.styles.disabled = props.styles.normal;
  props.buttonHeight = fui::clampI16(std::max<int>(
      48, screen.target().lineHeight(theme.bodyText.font) + metrics.optionPopupSelectionVPadding * 2));
  options[0].width = 48;
  options[1].width = 48;

  props.contentHeight = static_cast<int16_t>(sliderH + hintsH);
  // Bezel-safe area intersected with the front-button hint strip, which
  // follows the physical buttons: bottom in portrait, top inverted, logical
  // left landscape CW, right CCW. Touch boards report no strip.
  const Rect hints = UITheme::getInstance().getScreenSafeArea(mappedInput.getRenderer(), true, false);
  const fui::Rect bezel = screen.frame().safeRect();
  const int16_t usableLeft = static_cast<int16_t>(std::max<int>(bezel.x, hints.x));
  const int16_t usableTop = static_cast<int16_t>(std::max<int>(bezel.y, hints.y));
  const int16_t usableRight = static_cast<int16_t>(std::min<int>(bezel.right(), hints.x + hints.width));
  const int16_t usableBottom = static_cast<int16_t>(std::min<int>(bezel.bottom(), hints.y + hints.height));
  const fui::Rect screenRect = fui::Rect{usableLeft, usableTop, static_cast<int16_t>(usableRight - usableLeft),
                                         static_cast<int16_t>(usableBottom - usableTop)}
                                   .inset(fui::Insets{theme.spaceLg, 0, theme.spaceLg, 0});
  const int16_t width = fui::clampI16(screenRect.width - 2 * std::max<int>(
      theme.headerSidePadding, metrics.optionPopupDialogSideMargin));
  const int16_t confirmWidth = static_cast<int16_t>(
      width - 2 * innerPadding - options[0].width - options[1].width - 2 * props.gap - 8);
  if (touch) {
    props.buttonHeight = std::max<int16_t>(props.buttonHeight,
        fui::measureWrappedText(screen.target(), options[2].label, props.buttonText, confirmWidth).height + 8);
  }
  int16_t height = fui::optionDialogHeight(screen.target(), props, width);
  // Long translated captions yield lines before they can crowd a control.
  while (height > screenRect.height) {
    if (props.headlineText.maxLines > 1) {
      --props.headlineText.maxLines;
    } else if (props.titleText.maxLines > 1) {
      --props.titleText.maxLines;
    } else {
      break;
    }
    height = fui::optionDialogHeight(screen.target(), props, width);
  }
  height = fui::clampI16(height, 0, screenRect.height);
  const fui::Rect dialog = fui::centeredRect(screenRect, fui::Size{width, height});
  // Chrome guard first, controls after: routing scans newest-first, so the
  // buttons and slider win inside the dialog and the guard absorbs the rest —
  // same pattern as OptionPopup.
  screen.frame().hit(dialog, spec.chromeAction, 0, fui::InputTouch);
  const fui::Rect band = fui::optionDialog(screen.frame(), dialog, props);

  if (!touch) {
    // Two-line step hint (front buttons = fine step, side buttons = coarse
    // step) under the slider, preformatted by the caller so the layout
    // doesn't depend on a separator hidden in translated text.
    fui::TextStyle hint = theme.smallText;
    hint.align = fui::TextAlign::Center;
    const int16_t hintY = static_cast<int16_t>(band.y + sliderH + theme.spaceMd);
    if (spec.hintLine1) screen.target().text(fui::Rect{band.x, hintY, band.width, capLh}, spec.hintLine1, hint);
    if (spec.hintLine2) {
      screen.target().text(fui::Rect{band.x, static_cast<int16_t>(hintY + capLh + theme.spaceSm), band.width, capLh},
                           spec.hintLine2, hint);
    }
  }

  // Endpoints occupy separate lanes above the full-height drag target.
  const fui::Rect caption{band.x, band.y, static_cast<int16_t>((band.width - theme.spaceMd) / 2), capLh};
  fui::TextStyle endpoint = theme.smallText;
  if (spec.minLabel) screen.target().text(caption, spec.minLabel, endpoint);
  if (spec.maxLabel) {
    endpoint.align = fui::TextAlign::Right;
    screen.target().text(fui::Rect{static_cast<int16_t>(band.right() - caption.width), caption.y,
                                   caption.width, caption.height}, spec.maxLabel, endpoint);
  }

  fui::CapsuleSliderProps capsule;
  capsule.value = spec.value;
  capsule.max = spec.max;
  capsule.action = spec.sliderAction;
  capsule.radius = theme.capsuleRadius;
  capsule.stroke = 1;
  fui::capsuleSlider(screen.frame(),
                     fui::Rect{band.x, static_cast<int16_t>(band.y + capLh + theme.spaceMd), band.width, bandHeight},
                     capsule);
}
