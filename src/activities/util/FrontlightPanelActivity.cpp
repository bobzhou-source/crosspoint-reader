#include "FrontlightPanelActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_TOGGLE = 3;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 4;
constexpr fui::ActionId ACTION_WARMTH_STEP = 5;
constexpr fui::ActionId ACTION_TILE = 6;  // value = tile index

constexpr int16_t kSliderRowHeight = 48;
// One percent per press, on the -/+ buttons and on the physical Left/Right keys
// alike (both repeat while held), so a level can be set exactly.
constexpr int BRIGHTNESS_STEP = 1;
// The dimmest setting is 1%, not 0: turning the light off is what the lamp
// button next to the slider is for, so a 0% "on" level would only be a second,
// worse way to reach the same place.
constexpr uint8_t MIN_BRIGHTNESS = 1;

uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint8_t>(value);
}
}  // namespace

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FrontlightPanel", renderer, mappedInput), UiAppHost(renderer) {}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  // A stored 0% predates the 1% floor (or came from the web settings): show it
  // as the floor rather than a level the slider can no longer produce. onExit
  // persists that, which is the intent — 0 is not a brightness any more.
  brightness = std::max(MIN_BRIGHTNESS, Frontlight.brightness());
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  lightOnChanged = false;

  // Seed the touch tile's restore mode from the live setting, so toggling off
  // and back on within this session returns to the mode the user had.
  if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
    touchModeRestore = SETTINGS.touchReaderControls;
  }

  resetUi();
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TOGGLE, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.on(ACTION_TILE, &FrontlightPanelActivity::onTileEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  requestUpdate();
}

void FrontlightPanelActivity::persistLightSettings() {
  // brightness/warmth are always restored unconditionally on boot (see
  // main.cpp), so they never diverge from SETTINGS at onEnter() — comparing
  // against SETTINGS here only fires on a genuine user change. lightOn has
  // no such guarantee (see lightOnChanged's declaration), so it's gated on
  // the user actually having touched it this session instead.
  const bool changed = SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                       (lightOnChanged && SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (changed) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    if (lightOnChanged) SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
}

void FrontlightPanelActivity::onExit() {
  persistLightSettings();
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = std::max(MIN_BRIGHTNESS, percentFromPermille(event.dragPermille));
  Frontlight.setBrightness(self->brightness);
  if (!self->lightOn) {
    self->lightOn = true;
    self->lightOnChanged = true;
    Frontlight.setOn(true);
  }
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->warmth = percentFromPermille(event.dragPermille);
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onToggleEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleLight();
}

void FrontlightPanelActivity::onBrightnessStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value);
}

void FrontlightPanelActivity::onTileEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->runTile(event.value);
}

void FrontlightPanelActivity::runTile(const int idx) {
  switch (idx) {
    case 0:  // Night mode (inverted output polarity, applied to the whole UI)
      SETTINGS.screenInverted = SETTINGS.screenInverted ? 0 : 1;
      SETTINGS.saveToFile();
      // Inversion rewrites every pixel; take the clean waveform so the panel
      // does not keep a ghost of the old polarity.
      cleanRefreshPending = true;
      requestUpdate();
      break;
    case 1:  // Ghost-cleanup refresh of the whole frame
      // Refreshing with the panel still up would clean a frame the user is
      // about to dismiss anyway: drop the panel first and let the repaint of
      // the screen underneath carry the clean waveform instead.
      renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
      close();
      break;
    case 2:  // Cycle the reading orientation
      SETTINGS.orientation = static_cast<uint8_t>((SETTINGS.orientation + 1) % 4);
      SETTINGS.saveToFile();
      // Only the setting changes: turning the renderer cropped the portrait-only
      // screens the panel opens over. The reader reflows on its next loop().
      requestUpdate();
      break;
    case 3:  // Touch reader controls (for reading with the palm on the glass)
      // Toggles the existing Settings -> Controls option, nothing lower-level:
      // that setting only governs the reader's tap/swipe handling, so the
      // panel's own gestures (including the swipe that reopens it) keep
      // working while it is off. Off remembers the mode (Tap/Swipe/Inverted
      // Tap) so toggling back does not stomp the user's choice.
      if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
        touchModeRestore = SETTINGS.touchReaderControls;
        SETTINGS.touchReaderControls = CrossPointSettings::TOUCH_READER_OFF;
      } else {
        SETTINGS.touchReaderControls = touchModeRestore;
      }
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    default:
      break;
  }
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  int next = static_cast<int>(brightness) + delta;
  if (next < MIN_BRIGHTNESS) next = MIN_BRIGHTNESS;
  if (next > 100) next = 100;
  if (next == brightness) return;
  brightness = static_cast<uint8_t>(next);
  Frontlight.setBrightness(brightness);
  if (!lightOn) {
    lightOn = true;
    lightOnChanged = true;
    Frontlight.setOn(true);
  }
  requestUpdate();
}

void FrontlightPanelActivity::adjustWarmth(const int delta) {
  int next = static_cast<int>(warmth) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == warmth) return;
  warmth = static_cast<uint8_t>(next);
  Frontlight.setWarmth(warmth);
  requestUpdate();
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  lightOnChanged = true;
  Frontlight.setOn(lightOn);
  requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

bool FrontlightPanelActivity::handleHomeGesture() {
  close();
  return true;
}

void FrontlightPanelActivity::loop() {
  const auto touch = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    if (touch) {
      if (touch.event.dragPermille >= 0) draggingSlider = true;
      return;
    }
    // Swipe up dismisses the sheet, the way it was pulled down from the top
    // edge. draggingSlider keeps a fast slider flick from closing it.
    if (!draggingSlider && mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Up) {
      close();
      return;
    }
    // panelBottom > 0 guards the frame the sheet opens in: the release that
    // opened it (a status-bar tap) is still in the input snapshot when the panel
    // runs its first loop(), and panelBottom is only known once render() has
    // measured the layout — so at 0 that release read as "tapped below the
    // sheet" and closed it again before it was ever drawn.
    if (touch.snap.touchReleased && !draggingSlider && panelBottom > 0 && touch.snap.touchY >= panelBottom) {
      close();
      return;
    }
  }
  if (draggingSlider) {
    if (!touch.snap.touchHeld) draggingSlider = false;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleLight();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this] { adjustBrightness(-BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { adjustBrightness(BRIGHTNESS_STEP); });
}

void FrontlightPanelActivity::panelScreen(UiScreen& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::addSliderRow(UiScreen& screen, const char* label, const uint8_t value,
                                           const fui::ActionId sliderAction, const fui::ActionId stepAction,
                                           const bool showToggle) {
  // Live percentage readout. The row draws before this call returns
  // (immediate mode), so borrowing a stack buffer is safe.
  char pct[8];
  snprintf(pct, sizeof(pct), "%u%%", static_cast<unsigned>(value));

  const auto& theme = screen.theme();
  // Draw the caption in separate lanes so long translations cannot reach
  // the live value; sliderRow owns the identical control/hit geometry.
  rowProps.label = nullptr;
  rowProps.value = nullptr;
  rowProps.sliderValue = value;
  rowProps.sliderAction = sliderAction;
  rowProps.decrement = stepAction;
  rowProps.increment = stepAction;
  rowProps.decrementValue = -BRIGHTNESS_STEP;
  rowProps.incrementValue = BRIGHTNESS_STEP;
  if (showToggle) {
    // Lamp on/off after the +: the sliders set the level, this kills the light
    // outright. Filled glyph = on, outline = off.
    rowProps.toggleAction = ACTION_TOGGLE;
    rowProps.toggleIcon = fui::bitmapFromIcon(lightOn ? icon_sun_filled_32 : icon_sun_32);
  } else {
    rowProps.toggleAction = fui::NO_ACTION;
    rowProps.toggleIcon = fui::BitmapRef{};
  }
  // The Warmth row keeps the lamp column empty so its track and +/- line up
  // with the Brightness row above it, in portrait and landscape alike.
  rowProps.reserveToggleSlot = !showToggle;
  const fui::Rect row = screen.takeTop(fui::sliderRowHeight(screen.target(), rowProps, kSliderRowHeight),
                                       theme.spaceLg);
  const int16_t lineH = screen.target().lineHeight(rowProps.labelText.font);
  const int16_t valueW = screen.target().measureText(theme.smallText.font, "100%", theme.smallText).width;
  const int16_t labelW = std::max<int16_t>(0, row.width - valueW - theme.spaceMd);
  fui::TextStyle labelStyle = rowProps.labelText;
  labelStyle.align = UITheme::isRtl() ? fui::TextAlign::Right : fui::TextAlign::Left;
  fui::TextStyle valueStyle = theme.smallText;
  valueStyle.align = UITheme::isRtl() ? fui::TextAlign::Left : fui::TextAlign::Right;
  screen.target().text(fui::Rect{UITheme::isRtl() ? static_cast<int16_t>(row.right() - labelW) : row.x,
                                 row.y, labelW, lineH}, label, labelStyle);
  screen.target().text(fui::Rect{UITheme::isRtl() ? row.x : static_cast<int16_t>(row.right() - valueW),
                                 row.y, valueW, lineH}, pct, valueStyle);
  fui::sliderRow(screen.frame(), row, rowProps);
}

void FrontlightPanelActivity::buildPanelScreen(UiScreen& screen) {
  const auto& theme = screen.theme();

  const fui::Rect safe = screen.frame().safeRect();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int16_t headerH = static_cast<int16_t>(metrics.topPadding + metrics.headerHeight);
  rowProps.labelText = theme.bodyText;
  rowProps.valueText = theme.smallText;
  rowProps.buttonText = theme.bodyText;
  rowProps.buttonStyles = theme.button;
  rowProps.captionGap = theme.spaceMd;
  rowProps.gap = theme.spaceMd;
  rowProps.buttonRadius = theme.controlRadius;
  rowProps.capsuleRadius = theme.capsuleRadius;
  rowProps.stroke = 1;
  const int sliderCount = Frontlight.present() ? (Frontlight.hasColorTemperature() ? 2 : 1) : 0;
  const int16_t sliderH = fui::sliderRowHeight(screen.target(), rowProps, kSliderRowHeight);
  const int columns = safe.width > safe.height ? kTileCount : 2;
  const int16_t tileH = std::max<int16_t>(48, 2 * screen.target().lineHeight(theme.smallText.font) +
                                               2 * theme.spaceSm);
  const int16_t tileGap = theme.spaceMd;
  const int16_t gridH = mappedInput.hasTouch()
      ? fui::tileGridHeight(kTileCount, static_cast<uint8_t>(columns), tileH, tileGap) : 0;
  fui::SheetProps sheetProps;
  sheetProps.ruleWidth = 1;
  sheetProps.grabberHeight = 2;
  sheetProps.grabberMargin = theme.spaceMd;
  sheetProps.grabberInset = theme.spaceMd;
  const int16_t grabberBand = static_cast<int16_t>(
      sheetProps.grabberMargin + sheetProps.grabberHeight + sheetProps.grabberInset);
  const int16_t sheetH = static_cast<int16_t>(headerH + theme.spaceMd +
      sliderCount * (sliderH + theme.spaceLg) + gridH + grabberBand);
  screen.sheet(sheetProps, sheetH);
  panelBottom = safe.y + sheetH;

  const fui::Rect header = screen.takeTop(headerH, theme.spaceMd);
  GUI.drawHeader(renderer,
                 Rect{header.x, header.y + metrics.topPadding, header.width, metrics.headerHeight},
                 tr(STR_CAT_CONTROLS));
  screen.insetContent(fui::Insets{0, theme.headerSidePadding, 0, theme.headerSidePadding});

  if (Frontlight.present()) {
    addSliderRow(screen, tr(STR_BRIGHTNESS), brightness, ACTION_BRIGHTNESS, ACTION_BRIGHTNESS_STEP,
                 /*showToggle=*/true);
    if (Frontlight.hasColorTemperature()) {
      addSliderRow(screen, tr(STR_WARMTH), warmth, ACTION_WARMTH, ACTION_WARMTH_STEP, /*showToggle=*/false);
    }
  }

  // Quiet two-column actions in portrait, one row in landscape. A solid
  // underline marks a checked setting without inverting an entire tile.
  if (mappedInput.hasTouch()) {
    static constexpr StrId kOrientNames[4] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                              StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
    // The orientation tile is labelled with just the current mode ("Portrait"):
    // the mode names say what the tile is about on their own.
    const char* orientLabel = I18N.get(kOrientNames[SETTINGS.orientation % 4]);
    // "Touch On" / "Touch Off", from the existing state strings: the label
    // names the current state of the touch-reader-controls setting.
    const bool touchOn = SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF;
    char touchLabel[48];
    snprintf(touchLabel, sizeof(touchLabel), "%s %s", tr(STR_TOUCH_TOGGLE),
             I18N.get(touchOn ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF));

    const char* labels[kTileCount] = {tr(STR_NIGHT_MODE), tr(STR_FORCE_REFRESH), orientLabel, touchLabel};
    const bool checked[kTileCount] = {SETTINGS.screenInverted != 0, false, false, !touchOn};
    const fui::Rect grid = screen.takeTop(gridH);
    const int16_t tileW = static_cast<int16_t>((grid.width - (columns - 1) * tileGap) / columns);
    fui::TextStyle text = theme.smallText;
    text.align = UITheme::isRtl() ? fui::TextAlign::Right : fui::TextAlign::Left;
    text.maxLines = 2;
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    for (int id = 0; id < kTileCount; ++id) {
      const int column = UITheme::isRtl() ? columns - 1 - id % columns : id % columns;
      const fui::Rect tile{static_cast<int16_t>(grid.x + column * (tileW + tileGap)),
                           static_cast<int16_t>(grid.y + (id / columns) * (tileH + tileGap)), tileW, tileH};
      const fui::Rect labelRect =
          tile.inset(fui::Insets{theme.spaceSm, theme.spaceSm, theme.spaceSm, theme.spaceSm});
      fui::TextStyle lineStyle = text;
      lineStyle.align = fui::TextAlign::Left;
      lineStyle.maxLines = 1;
      fui::layoutText(screen.target(), labelRect, labels[id], text, [&](const char* line, fui::Rect rect) {
        screen.target().text(rect, line, lineStyle);
      });
      if (checked[id]) {
        screen.target().fill(fui::Rect{static_cast<int16_t>(tile.x + theme.spaceSm),
                                       static_cast<int16_t>(tile.bottom() - 2),
                                       static_cast<int16_t>(tile.width - 2 * theme.spaceSm), 2}, ink);
      }
      screen.frame().hit(tile, ACTION_TILE, static_cast<int16_t>(id), fui::InputTouch);
    }
  }
}

void FrontlightPanelActivity::render(RenderLock&&) {
  // The sheet publishes the same measured geometry for paint and touch.
  renderUi();

  // A tile that rewrote the whole frame (night mode) re-drives every pixel
  // once; ordinary repaints stay on the fast path. HALF: strong enough to
  // flip the whole frame's polarity without the FULL waveform's blackout
  // flash. Any faint residue clears with the panel's dedicated refresh tile
  // or the next scheduled clean refresh.
  renderer.displayBuffer(cleanRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  cleanRefreshPending = false;
}
