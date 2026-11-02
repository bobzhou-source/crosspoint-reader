#include "TxtReaderPageSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "components/UITheme.h"
#include "components/UiSliderDialog.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_SLIDER = 1;
constexpr fui::ActionId ACTION_STEP = 2;
constexpr fui::ActionId ACTION_OK = 4;
constexpr fui::ActionId ACTION_CHROME = 5;  // absorbs taps on the dialog body; no handler
// Fine/coarse page steps (buttons and -/+ tap zones).
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
}  // namespace

TxtReaderPageSelectionActivity::TxtReaderPageSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                               const int initialPage, const int pageCount,
                                                               const bool indexing)
    : Activity("TxtReaderPageSelection", renderer, mappedInput),
      UiAppHost(renderer),
      page(std::clamp(initialPage, 1, std::max(pageCount, 1))),
      pageCount(std::max(pageCount, 1)),
      indexing(indexing) {}

void TxtReaderPageSelectionActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(ACTION_SLIDER, &TxtReaderPageSelectionActivity::onSliderEvent, this);
  app.on(ACTION_STEP, &TxtReaderPageSelectionActivity::onStepEvent, this);
  app.on(ACTION_OK, &TxtReaderPageSelectionActivity::onOkEvent, this);
  app.setScreen(&TxtReaderPageSelectionActivity::pageScreen, this);
  requestUpdate();
}

void TxtReaderPageSelectionActivity::onExit() { Activity::onExit(); }

void TxtReaderPageSelectionActivity::adjustPage(const int delta) { setPage(page + delta); }

void TxtReaderPageSelectionActivity::setPage(const int value) {
  const int clamped = std::clamp(value, 1, pageCount);
  if (clamped == page) return;
  page = clamped;
  requestUpdate();
}

void TxtReaderPageSelectionActivity::onSliderEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<TxtReaderPageSelectionActivity*>(user);
  if (event.dragPermille < 0) return;
  self->setPage(1 + (static_cast<int>(event.dragPermille) * (self->pageCount - 1) + 500) / 1000);
}

void TxtReaderPageSelectionActivity::onStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<TxtReaderPageSelectionActivity*>(user)->adjustPage(event.value * kSmallStep);
}

void TxtReaderPageSelectionActivity::onOkEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<TxtReaderPageSelectionActivity*>(user);
  self->app.clearTapFlash();  // the tap leaves this screen
  self->confirm();
}

void TxtReaderPageSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void TxtReaderPageSelectionActivity::confirm() {
  setResult(PageResult{static_cast<uint32_t>(page)});
  finish();
}

void TxtReaderPageSelectionActivity::loop() {
  // Touch first: a slider drag's release can also read as a swipe or a back
  // gesture, so the drag consumes it (see EpubReaderPercentSelectionActivity).
  const auto route = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (route.routed && app.invalidated()) requestUpdate();
  if (route) {
    if (route.event.dragPermille >= 0) draggingSlider = true;
    return;
  }
  if (routingReady() && draggingSlider) {
    if (!route.snap.touchHeld) draggingSlider = false;
    return;
  }
  // Tap released outside the dialog: cancel.
  if (route.routed && route.snap.touchReleased && route.snap.touchX >= 0) {
    cancel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Right) {
    adjustPage(kLargeStep);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Left) {
    adjustPage(-kLargeStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirm();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustPage(-kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustPage(kSmallStep); });

  // Edge-button boards put BTN_UP on the left edge: the left button goes back there.
  const int upDelta = gpio.hasEdgeSideButtons() ? -kLargeStep : kLargeStep;
  const int downDelta = gpio.hasEdgeSideButtons() ? kLargeStep : -kLargeStep;
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this, upDelta] { adjustPage(upDelta); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                       [this, downDelta] { adjustPage(downDelta); });
}

void TxtReaderPageSelectionActivity::pageScreen(UiScreen& screen, void* user) {
  static_cast<TxtReaderPageSelectionActivity*>(user)->buildPageScreen(screen);
}

void TxtReaderPageSelectionActivity::buildPageScreen(UiScreen& screen) {
  // An unfinished index supplies an estimated total, not an indexed-page limit.
  const char* estimate = indexing ? "~" : "";
  char readout[32];
  snprintf(readout, sizeof(readout), "%d / %s%d", page, estimate, pageCount);
  char maxLabel[16];
  snprintf(maxLabel, sizeof(maxLabel), "%s%d", estimate, pageCount);
  char hint1[64];
  snprintf(hint1, sizeof(hint1), "%s %d", I18N.get(StrId::STR_STEP_HINT_FRONT), kSmallStep);
  char hint2[64];
  snprintf(hint2, sizeof(hint2), "%s %d", I18N.get(StrId::STR_STEP_HINT_SIDE), kLargeStep);

  UiSliderDialogSpec spec;
  spec.title = tr(STR_GO_TO_PAGE);
  spec.readout = readout;
  spec.value = page - 1;
  spec.max = std::max(pageCount - 1, 1);
  spec.minLabel = "1";
  spec.maxLabel = maxLabel;
  spec.sliderAction = ACTION_SLIDER;
  spec.stepAction = ACTION_STEP;
  spec.okAction = ACTION_OK;
  spec.chromeAction = ACTION_CHROME;
  spec.hintLine1 = hint1;
  spec.hintLine2 = hint2;
  buildSliderDialogScreen(screen, uiTarget, mappedInput, spec);
}

void TxtReaderPageSelectionActivity::render(RenderLock&&) {
  // A popup over the page it opened from: no clearScreen.
  renderUi();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
