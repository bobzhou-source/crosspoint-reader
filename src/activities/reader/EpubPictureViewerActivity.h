#pragma once

#include <GfxRenderer.h>

#include <string>

#include "activities/Activity.h"

// Full-screen view of one picture tapped on the reading page. The source file
// (not the page-size .pxc) is decoded once at screen resolution, fit to the
// screen and turned a quarter when that shows a landscape picture larger, then
// shown in grayscale. The decoded frame is kept next to the source as a .pxv
// so reopening skips the decode. Any tap or button returns to the page, which
// the reader re-renders from its still-loaded section.
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
  bool painted = false;

  void showMessage(const char* message);
};
