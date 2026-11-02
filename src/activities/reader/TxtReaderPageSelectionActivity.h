#pragma once

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Slider dialog choosing a page of a TXT book. While indexing, the range end
// reads "~N" for its estimated total. The reader indexes through the chosen
// page after confirmation. Finishes with PageResult{page}, 1-based.
class TxtReaderPageSelectionActivity final : public Activity, private UiAppHost {
 public:
  explicit TxtReaderPageSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int initialPage,
                                          int pageCount, bool indexing);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static void pageScreen(UiScreen& screen, void* user);
  static void onSliderEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onOkEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildPageScreen(UiScreen& screen);
  void cancel();
  void confirm();

  // Current page (1..pageCount) shown on the slider.
  int page = 1;
  int pageCount = 1;
  bool indexing = false;

  ButtonNavigator buttonNavigator;

  // Swallow the swipe/tap fallout of a slider drag so its release can't trigger
  // the back gesture and cancel the dialog, or step the page as a swipe.
  bool draggingSlider = false;

  // Moves by `delta` pages, clamped to the displayed range.
  void adjustPage(int delta);
  void setPage(int value);
};
