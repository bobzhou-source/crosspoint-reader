#pragma once

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Top-anchored controls opened by a top-edge down-swipe, a status-bar tap,
// or the Control Center button action. Brightness and warmth stay live while
// the sheet is open; quick actions retain their existing settings routes.
// The sheet's measured free edge is also the outside-tap dismissal boundary.
class FrontlightPanelActivity final : public Activity, private UiAppHost {
  ButtonNavigator buttonNavigator;

  uint8_t brightness = 60;
  uint8_t warmth = 50;
  bool lightOn = false;
  // lightOn is seeded from the live hardware state (Frontlight.isOn()), which
  // legitimately diverges from the saved SETTINGS.frontlightOn preference —
  // e.g. after a wake with frontlightRestoreOnWake off, the light stays off
  // live while the saved "was on" preference is deliberately kept (see
  // main.cpp's restoreLightOn). brightness/warmth have no such divergence
  // (always restored unconditionally on boot), so only lightOn needs a
  // touched-by-the-user flag: onExit() must not persist a mirror that never
  // reflected user intent in the first place.
  bool lightOnChanged = false;
  bool draggingSlider = false;
  // The touch tile toggles SETTINGS.touchReaderControls between off and this
  // remembered mode, so a Swipe or Inverted Tap user gets their mode back
  // rather than the Tap default. Seeded from the setting in onEnter().
  uint8_t touchModeRestore = CrossPointSettings::TOUCH_READER_ON;
  int panelBottom = 0;

  // Fixed quick-action order: night mode, refresh, orientation, touch.
  static constexpr int kTileCount = 4;

  // SliderRowProps embeds a StyleSet; reuse it rather than putting it on
  // the constrained render-task stack.
  freeink::ui::SliderRowProps rowProps;

  static void panelScreen(UiScreen& screen, void* user);
  static void onBrightnessEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onToggleEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onBrightnessStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onTileEvent(const freeink::ui::ActionEvent& event, void* user);

  void buildPanelScreen(UiScreen& screen);
  // Caption and value above [-] [draggable level] [+], with a lamp toggle
  // after the brightness row's +. Painting and routing share the row rect.
  void addSliderRow(UiScreen& screen, const char* label, uint8_t value, freeink::ui::ActionId sliderAction,
                    freeink::ui::ActionId stepAction, bool showToggle);
  void adjustBrightness(int delta);
  void adjustWarmth(int delta);
  void toggleLight();
  void runTile(int idx);
  // Copy the panel's live brightness/warmth/lightOn into SETTINGS and save if
  // anything actually changed. onExit() runs it on every way out.
  void persistLightSettings();
  void close();

  // One-shot: a tile that rewrote the whole frame (night mode) re-drives it
  // with the ghost-cleanup waveform on the next render. The "refresh" tile does
  // not use this — it closes the panel and promotes the repaint underneath
  // instead (GfxRenderer::promoteNextRefresh).
  bool cleanRefreshPending = false;

 public:
  explicit FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;
};
