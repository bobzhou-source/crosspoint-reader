#include "EpubPictureViewerActivity.h"

#include <Epub/PagePicture.h>
#include <Epub/converters/DirectPixelWriter.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageDimsProbe.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <utility>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

static_assert(page_picture::MAX_SOURCE_PIXELS == ImageToFramebufferDecoder::MAX_SOURCE_PIXELS,
              "viewer refusal must match the page decoders");
static_assert(page_picture::MAX_SOURCE_DIMENSION == ImageToFramebufferDecoder::MAX_SOURCE_DIMENSION,
              "viewer refusal must match the page decoders");

// JPEG APPn segments (EXIF, ICC) precede the frame header; stop reading if a
// header has not turned up by then.
constexpr size_t MAX_PROBE_BYTES = 1024 * 1024;
constexpr size_t PROBE_CHUNK_BYTES = 128;
// Rows of the decoded frame read per SD access when replaying a plane.
constexpr size_t REPLAY_READ_BYTES = 4096;

// The full-screen frame lives beside the source image in the book cache (and
// goes with it when the cache is deleted). A distinct extension: page caches
// are always "<stem>.pxc".
std::string viewCachePath(const std::string& imagePath) { return imagePath + ".pxv"; }

bool probeSourceDimensions(const std::string& path, uint32_t& width, uint32_t& height) {
  HalFile file;
  if (!Storage.openFileForRead("PIC", path, file)) return false;
  ImageDimsProbe probe;
  uint8_t chunk[PROBE_CHUNK_BYTES];
  size_t total = 0;
  while (total < MAX_PROBE_BYTES) {
    const int count = file.read(chunk, sizeof(chunk));
    if (count <= 0) break;
    total += static_cast<size_t>(count);
    // A short write means the probe is finished (found or failed).
    if (probe.write(chunk, static_cast<size_t>(count)) < static_cast<size_t>(count)) break;
  }
  file.close();
  return probe.getRawDimensions(width, height);
}

// Opens the frame cache positioned at its pixels when it holds exactly `fit`.
bool openViewCache(const std::string& cachePath, const page_picture::FitLayout& fit, HalFile& file) {
  if (!Storage.exists(cachePath.c_str())) return false;
  if (!Storage.openFileForRead("PIC", cachePath, file)) return false;
  uint16_t width = 0;
  uint16_t height = 0;
  const size_t expectedSize = 4 + static_cast<size_t>((fit.width + 3) / 4) * static_cast<size_t>(fit.height);
  if (file.read(&width, 2) != 2 || file.read(&height, 2) != 2 || width != fit.width || height != fit.height ||
      file.size() < expectedSize) {
    file.close();
    return false;
  }
  return true;
}

// Writes the cached 2-bit frame into the current render mode's plane.
bool drawFromViewCache(GfxRenderer& renderer, const std::string& cachePath, const page_picture::FitLayout& fit) {
  HalFile file;
  if (!openViewCache(cachePath, fit, file)) return false;
  const int bytesPerRow = (fit.width + 3) / 4;
  const int rowsPerRead = std::max(1, static_cast<int>(REPLAY_READ_BYTES) / bytesPerRow);
  auto buffer = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(rowsPerRead) * bytesPerRow);
  if (!buffer) {
    LOG_ERR("PIC", "OOM: replay buffer");
    file.close();
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);
  for (int row = 0; row < fit.height; row += rowsPerRead) {
    const int rows = std::min(rowsPerRead, fit.height - row);
    const size_t bytes = static_cast<size_t>(rows) * bytesPerRow;
    if (file.read(buffer.get(), bytes) != static_cast<int>(bytes)) {
      LOG_ERR("PIC", "Frame cache read failed at row %d", row);
      file.close();
      return false;
    }
    for (int r = 0; r < rows; r++) {
      const uint8_t* src = buffer.get() + static_cast<size_t>(r) * bytesPerRow;
      pw.beginRow(fit.y + row + r);
      for (int col = 0; col < fit.width; col++) {
        pw.writePixel(fit.x + col, (src[col >> 2] >> (6 - (col & 3) * 2)) & 0x03);
      }
    }
  }
  file.close();
  return true;
}

}  // namespace

EpubPictureViewerActivity::EpubPictureViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     std::string imagePath)
    : Activity("EpubPictureViewer", renderer, mappedInput), imagePath(std::move(imagePath)) {}

void EpubPictureViewerActivity::onEnter() {
  Activity::onEnter();
  readerOrientation = renderer.getOrientation();
  painted = false;
  requestUpdate();
}

void EpubPictureViewerActivity::onExit() {
  Activity::onExit();
  // A rotated picture turned only the renderer; the reader repaints its page
  // in its own orientation.
  renderer.setOrientation(readerOrientation);
  renderer.setRenderMode(GfxRenderer::BW);
}

void EpubPictureViewerActivity::loop() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasAnyReleased() || mappedInput.wasScreenTapped(x, y) || mappedInput.wasBackGesture()) {
    finish();
  }
}

void EpubPictureViewerActivity::showMessage(const char* message) {
  renderer.setOrientation(readerOrientation);
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  Rect body = UITheme::getContentBounds(renderer, false);
  body.height = std::max(0, body.height - 64);
  UITheme::drawCenteredWrappedText(renderer, body, UI_12_FONT_ID, message, 8);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void EpubPictureViewerActivity::render(RenderLock&&) {
  if (painted) return;
  painted = true;
  renderer.setRenderMode(GfxRenderer::BW);

  uint32_t sourceWidth = 0;
  uint32_t sourceHeight = 0;
  const auto check = probeSourceDimensions(imagePath, sourceWidth, sourceHeight)
                         ? page_picture::checkSource(sourceWidth, sourceHeight)
                         : page_picture::SourceCheck::Invalid;
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (check == page_picture::SourceCheck::TooLarge) {
    LOG_INF("PIC", "Picture too large: %ux%u %s", sourceWidth, sourceHeight, imagePath.c_str());
    showMessage(tr(STR_PICTURE_TOO_LARGE));
    return;
  }
  if (check != page_picture::SourceCheck::Ok || !decoder) {
    LOG_ERR("PIC", "Cannot read picture: %s", imagePath.c_str());
    showMessage(tr(STR_FILE_OPEN_FAILED));
    return;
  }

  const page_picture::FitLayout fit =
      page_picture::fitToScreen(static_cast<int>(sourceWidth), static_cast<int>(sourceHeight),
                                renderer.getScreenWidth(), renderer.getScreenHeight());
  const std::string cachePath = viewCachePath(imagePath);
  HalFile cached;
  const bool haveFrame = openViewCache(cachePath, fit, cached);
  if (haveFrame) cached.close();
  // First view of this picture decodes the source; say so over the page.
  if (!haveFrame) GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  if (fit.rotate) {
    renderer.setOrientation(
        static_cast<GfxRenderer::Orientation>(page_picture::quarterTurnOrientation(readerOrientation)));
  }
  renderer.clearScreen();
  bool frameCached = haveFrame && drawFromViewCache(renderer, cachePath, fit);
  if (!frameCached) {
    renderer.clearScreen();
    RenderConfig config;
    config.x = fit.x;
    config.y = fit.y;
    config.maxWidth = fit.width;
    config.maxHeight = fit.height;
    config.useGrayscale = true;
    config.useDithering = true;
    config.useExactDimensions = true;
    config.cachePath = cachePath;
    if (!decoder->decodeToFramebuffer(imagePath, renderer, config)) {
      LOG_ERR("PIC", "Decode failed: %s", imagePath.c_str());
      if (Storage.exists(cachePath.c_str())) Storage.remove(cachePath.c_str());
      showMessage(decoder->lastDecodeTooLarge() ? tr(STR_PICTURE_TOO_LARGE) : tr(STR_FILE_OPEN_FAILED));
      return;
    }
    HalFile written;
    frameCached = openViewCache(cachePath, fit, written);
    if (frameCached) written.close();
  }
  renderer.preserveImagePolarity(fit.x, fit.y, fit.width, fit.height);
  LOG_DBG("PIC", "%ux%u -> %dx%d at %d,%d%s", sourceWidth, sourceHeight, fit.width, fit.height, fit.x, fit.y,
          fit.rotate ? " (rotated)" : "");

  // Without the frame cache (SD full) the gray planes cannot be replayed; the
  // dithered B/W frame from the decode is still the whole picture.
  if (!frameCached) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  // Grayscale: B/W base, then the two planes, then the B/W frame rebuilt for
  // later differential updates (BmpViewerActivity's sequence).
  const bool absolute = renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
  if (absolute) {
    if (!renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute)) {
      // A refused base displayed nothing.
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      return;
    }
  } else {
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  }
  bool planesReady = true;
  for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    renderer.clearScreen(absolute ? 0xFF : 0x00);
    renderer.setRenderMode(mode);
    if (!drawFromViewCache(renderer, cachePath, fit)) {
      planesReady = false;
      break;
    }
    if (mode == GfxRenderer::GRAYSCALE_LSB) {
      renderer.copyGrayscaleLsbBuffers();
    } else {
      renderer.copyGrayscaleMsbBuffers();
    }
  }
  if (planesReady) renderer.displayGrayBuffer();

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  if (!drawFromViewCache(renderer, cachePath, fit)) {
    LOG_ERR("PIC", "Frame cache unreadable after the gray planes");
  }
  renderer.preserveImagePolarity(fit.x, fit.y, fit.width, fit.height);
  renderer.cleanupGrayscaleWithFrameBuffer();
  if (!planesReady) renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
