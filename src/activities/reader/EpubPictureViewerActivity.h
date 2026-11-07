#pragma once

#include <Epub/PagePicture.h>
#include <GfxRenderer.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "activities/Activity.h"

// Full-screen view of one picture tapped on the reading page. The source file
// (not the page-size .pxc) is decoded once at screen resolution, fit to the
// screen in the reading orientation, and shown in grayscale. A direction
// button turns the picture a quarter (and back). The decoded frame is kept
// next to the source as a .pxv2 (.pxv2r turned) so reopening skips the decode.
// A tap, the Back swipe, or Back/Confirm returns to the page, which the reader
// re-renders from its still-loaded section; a tap only counts once the frame
// is up (see page_picture::ViewerCloseGate), so a double tap opens cleanly.
class EpubPictureViewerActivity final : public Activity {
 public:
  EpubPictureViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string imagePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string imagePath;
  GfxRenderer::Orientation readerOrientation = GfxRenderer::Portrait;
  // Loop sets the wanted turn; render paints when it differs from the shown one.
  std::atomic<bool> rotated{false};
  int8_t paintedRotation = -1;  // render task only; -1 = nothing painted yet
  // millis() when render last finished a frame (0 = none yet), for the gate.
  std::atomic<uint32_t> shownAtMs{0};
  uint32_t gateShownAtMs = 0;
  page_picture::ViewerCloseGate closeGate;

  void showMessage(const char* message);
  void paint(bool rotate);
  void markShown();
};
