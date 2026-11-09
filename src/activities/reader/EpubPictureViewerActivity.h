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
// button turns the picture a quarter (and back). The reader's orientation
// head (page_picture::RotationAdvisor, LiquidDisplay CfC y4) picks the
// opening turn and learns from every button turn. The decoded frame is kept
// next to the source as a .pxv4 (.pxv4r turned) so reopening skips the decode;
// v4 frames carry the LiquidDisplay dither head (contrast gain, error bleed,
// unsharp mask, paper-white snap).
// A tap, the Back swipe, or Back/Confirm returns to the page, which the reader
// re-renders from its still-loaded section; a tap only counts once the frame
// is up (see page_picture::ViewerCloseGate), so a double tap opens cleanly.
// Closing reports a PictureViewerResult (dwell, area, dither, tiles) so the
// reader's refresh policy can book the image exposure.
class EpubPictureViewerActivity final : public Activity {
 public:
  EpubPictureViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string imagePath,
                            page_picture::RotationAdvisor rotationAdvisor = {});

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string imagePath;
  GfxRenderer::Orientation readerOrientation = GfxRenderer::Portrait;
  page_picture::RotationAdvisor rotationAdvisor;
  // Source size (onEnter's probe; 0 x 0 when unreadable) and the reading
  // screen it is fit to.
  uint32_t sourceWidth = 0;
  uint32_t sourceHeight = 0;
  uint16_t viewWidth = 0;
  uint16_t viewHeight = 0;
  // Loop sets the wanted turn; render paints when it differs from the shown one.
  std::atomic<bool> rotated{false};
  int8_t paintedRotation = -1;  // render task only; -1 = nothing painted yet
  // millis() when render last finished a frame (0 = none yet), for the gate.
  std::atomic<uint32_t> shownAtMs{0};
  uint32_t gateShownAtMs = 0;
  page_picture::ViewerCloseGate closeGate;
  // Exposure of the shown picture (render task writes, loop reads under the
  // render lock): when it first went up, its screen share, mid-gray share and
  // reader-orientation tiles (0 = all, e.g. when turned).
  uint32_t exposureStartMs = 0;
  uint16_t exposureAreaPermille = 0;
  uint16_t exposureDitherPermille = 0;
  uint64_t exposureTiles = 0;

  void showMessage(const char* message);
  void paint(bool rotate);
  void markShown();
  void finishWithExposure();
};
