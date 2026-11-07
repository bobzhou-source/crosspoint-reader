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

// Finger slop around a tappable picture: a tap this close to its edge still
// opens it instead of turning the page.
constexpr int TAP_PAD = 8;

// Gap the full-screen viewer leaves around a picture that fills the screen.
constexpr int VIEW_MARGIN = 4;

// Largest enlargement the viewer applies: a tiny picture blown up further
// only shows its pixels larger.
constexpr float MAX_UPSCALE = 3.0f;

inline bool isTappable(const PagePicture& picture) {
  return picture.width >= MIN_TAP_EXTENT && picture.height >= MIN_TAP_EXTENT;
}

// Index of the tappable picture under screen point (x, y), or -1. The
// picture's box grows by TAP_PAD on every side; the first hit wins.
inline int pictureAtPoint(const std::vector<PagePicture>& pictures, const int x, const int y, const int marginLeft,
                          const int marginTop) {
  const int pageX = x - marginLeft;
  const int pageY = y - marginTop;
  for (size_t i = 0; i < pictures.size(); i++) {
    const PagePicture& picture = pictures[i];
    if (!isTappable(picture)) continue;
    if (pageX >= picture.x - TAP_PAD && pageX < picture.x + picture.width + TAP_PAD && pageY >= picture.y - TAP_PAD &&
        pageY < picture.y + picture.height + TAP_PAD) {
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

// Fit the whole source picture inside the screen less VIEW_MARGIN, centred,
// scaling down or up (at most MAX_UPSCALE). The picture keeps the reading
// orientation unless `rotate` asks for the quarter turn (the viewer's
// direction buttons toggle it).
inline FitLayout fitToScreen(const int sourceWidth, const int sourceHeight, const int screenWidth,
                             const int screenHeight, const bool rotate = false) {
  FitLayout layout;
  if (sourceWidth <= 0 || sourceHeight <= 0 || screenWidth <= 0 || screenHeight <= 0) return layout;

  layout.rotate = rotate;
  const int frameWidth = rotate ? screenHeight : screenWidth;
  const int frameHeight = rotate ? screenWidth : screenHeight;
  const int usableWidth = std::max(1, frameWidth - 2 * VIEW_MARGIN);
  const int usableHeight = std::max(1, frameHeight - 2 * VIEW_MARGIN);
  const float scale = std::min({static_cast<float>(usableWidth) / static_cast<float>(sourceWidth),
                                static_cast<float>(usableHeight) / static_cast<float>(sourceHeight), MAX_UPSCALE});
  layout.width = std::clamp(static_cast<int>(static_cast<float>(sourceWidth) * scale + 0.5f), 1, usableWidth);
  layout.height = std::clamp(static_cast<int>(static_cast<float>(sourceHeight) * scale + 0.5f), 1, usableHeight);
  layout.x = (frameWidth - layout.width) / 2;
  layout.y = (frameHeight - layout.height) / 2;
  return layout;
}

// When a tap may close the full-screen viewer. A double tap on a page picture
// opens the viewer with its first tap; its second must not close it again.
// Taps count only from OPEN_DEBOUNCE_MS after the frame is on screen, and only
// once the screen has been seen untouched since then, so the closing tap is a
// fresh contact.
struct ViewerCloseGate {
  static constexpr uint32_t OPEN_DEBOUNCE_MS = 350;

  // The viewer finished painting a frame at `now` (ms).
  void frameShown(const uint32_t now) {
    shown = true;
    shownAt = now;
    idleSeen = false;
  }

  // Called every loop. `touching`: a finger is on the screen now. `released`:
  // a contact ended this frame (tap or swipe). True when it should close.
  bool closes(const uint32_t now, const bool touching, const bool released) {
    if (!shown || now - shownAt < OPEN_DEBOUNCE_MS) return false;
    const bool close = released && idleSeen;
    if (!touching && !released) idleSeen = true;
    return close;
  }

  bool shown = false;
  bool idleSeen = false;
  uint32_t shownAt = 0;
};

// The quarter turn used for a rotated picture: portrait readings turn to the
// landscape next in GfxRenderer::Orientation order, landscape readings back.
// Values follow GfxRenderer::Orientation (Portrait, LandscapeClockwise,
// PortraitInverted, LandscapeCounterClockwise).
inline uint8_t quarterTurnOrientation(const uint8_t orientation) {
  return (orientation & 1u) == 0 ? static_cast<uint8_t>(orientation + 1) : static_cast<uint8_t>(orientation - 1);
}

}  // namespace page_picture
