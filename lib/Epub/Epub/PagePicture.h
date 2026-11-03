#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

// A picture on the reading page, in page-relative pixels (the frame PageLink
// uses; the reader adds its oriented margins when hit-testing). `element` is
// the PageImage's index in Page::elements, so a tap can reload that one block
// on demand instead of every page turn keeping image paths around.
struct PagePicture {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
  uint16_t element = 0;
};

namespace page_picture {

// Pictures smaller than this in either direction (drop caps, dingbats, rule
// ornaments) stay part of the page, so taps on them still turn pages.
constexpr int MIN_TAP_EXTENT = 48;

// Source limits of the page decoders (ImageToFramebufferDecoder): the viewer
// decodes from the same source, so it refuses the same pictures up front.
constexpr int64_t MAX_SOURCE_DIMENSION = INT16_MAX;
constexpr int64_t MAX_SOURCE_PIXELS = 8388608;

// Auto-rotation must enlarge the picture by at least this factor; a near-square
// picture is not worth turning the device for.
constexpr float MIN_ROTATE_GAIN = 1.15f;

inline bool isTappable(const PagePicture& picture) {
  return picture.width >= MIN_TAP_EXTENT && picture.height >= MIN_TAP_EXTENT;
}

// Index of the tappable picture under screen point (x, y), or -1. No finger
// slop: the picture's own box is the target, so taps beside it keep the
// page-turn and menu zones exactly as before.
inline int pictureAtPoint(const std::vector<PagePicture>& pictures, const int x, const int y, const int marginLeft,
                          const int marginTop) {
  const int pageX = x - marginLeft;
  const int pageY = y - marginTop;
  for (size_t i = 0; i < pictures.size(); i++) {
    const PagePicture& picture = pictures[i];
    if (!isTappable(picture)) continue;
    if (pageX >= picture.x && pageX < picture.x + picture.width && pageY >= picture.y &&
        pageY < picture.y + picture.height) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

enum class SourceCheck : uint8_t { Ok, TooLarge, Invalid };

inline SourceCheck checkSource(const int64_t width, const int64_t height) {
  if (width <= 0 || height <= 0) return SourceCheck::Invalid;
  if (width > MAX_SOURCE_DIMENSION || height > MAX_SOURCE_DIMENSION || width * height > MAX_SOURCE_PIXELS) {
    return SourceCheck::TooLarge;
  }
  return SourceCheck::Ok;
}

// Where the picture lands on the viewer screen. `rotate` means the viewer
// shows it a quarter turn from the reading orientation, where screenWidth and
// screenHeight swap; x/y/width/height are in that frame.
struct FitLayout {
  bool rotate = false;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// Fit the whole source picture to the screen, scaling up or down, centred.
// A picture that fills the screen clearly better a quarter turn away (a
// landscape picture on a portrait screen) is laid out rotated.
inline FitLayout fitToScreen(const int sourceWidth, const int sourceHeight, const int screenWidth,
                             const int screenHeight) {
  FitLayout layout;
  if (sourceWidth <= 0 || sourceHeight <= 0 || screenWidth <= 0 || screenHeight <= 0) return layout;

  const auto scaleFor = [&](const int width, const int height) {
    return std::min(static_cast<float>(width) / static_cast<float>(sourceWidth),
                    static_cast<float>(height) / static_cast<float>(sourceHeight));
  };
  const float upright = scaleFor(screenWidth, screenHeight);
  const float turned = scaleFor(screenHeight, screenWidth);
  layout.rotate = turned > upright * MIN_ROTATE_GAIN;

  const int frameWidth = layout.rotate ? screenHeight : screenWidth;
  const int frameHeight = layout.rotate ? screenWidth : screenHeight;
  const float scale = layout.rotate ? turned : upright;
  layout.width = std::clamp(static_cast<int>(static_cast<float>(sourceWidth) * scale + 0.5f), 1, frameWidth);
  layout.height = std::clamp(static_cast<int>(static_cast<float>(sourceHeight) * scale + 0.5f), 1, frameHeight);
  layout.x = (frameWidth - layout.width) / 2;
  layout.y = (frameHeight - layout.height) / 2;
  return layout;
}

// The quarter turn used for a rotated picture: portrait readings turn to the
// landscape next in GfxRenderer::Orientation order, landscape readings back.
// Values follow GfxRenderer::Orientation (Portrait, LandscapeClockwise,
// PortraitInverted, LandscapeCounterClockwise).
inline uint8_t quarterTurnOrientation(const uint8_t orientation) {
  return (orientation & 1u) == 0 ? static_cast<uint8_t>(orientation + 1) : static_cast<uint8_t>(orientation - 1);
}

}  // namespace page_picture
