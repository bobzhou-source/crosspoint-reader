#include "PagePrerender.h"

#include <Logging.h>

#include <utility>

bool PagePrerender::ensureBuffers(const size_t bufferBytes, const bool wantPlanes) {
  if (bufferBytes == 0) return false;
  if (bytes != bufferBytes) {
    // Panel geometry cannot change at runtime; this only guards a first call.
    slot.invalidate();
    bwBuffer.reset();
    lsbBuffer.reset();
    msbBuffer.reset();
    bytes = bufferBytes;
  }

  const bool needBw = !bwBuffer;
  const bool needPlanes = wantPlanes && (!lsbBuffer || !msbBuffer);
  if (!needBw && !needPlanes) return true;

  const size_t count = (needBw ? 1 : 0) + (needPlanes ? 2 : 0);
  const HalMemory::HeapStats psram = HalMemory::getPsramHeap();
  if (!page_prerender::psramAdmits(psram.freeBytes, psram.largestBlockBytes, bufferBytes, count)) {
    LOG_DBG("PPR", "Skip: PSRAM free=%u largest=%u need=%ux%u+reserve", static_cast<unsigned>(psram.freeBytes),
            static_cast<unsigned>(psram.largestBlockBytes), static_cast<unsigned>(bufferBytes),
            static_cast<unsigned>(count));
    return false;
  }

  HalMemory::PsramBuffer bwNew;
  HalMemory::PsramBuffer lsbNew;
  HalMemory::PsramBuffer msbNew;
  if (needBw) bwNew = HalMemory::allocatePsram(bufferBytes);
  if (needPlanes) {
    lsbNew = HalMemory::allocatePsram(bufferBytes);
    msbNew = HalMemory::allocatePsram(bufferBytes);
  }
  if ((needBw && !bwNew) || (needPlanes && (!lsbNew || !msbNew))) {
    LOG_ERR("PPR", "PSRAM allocation failed (%u buffers of %u)", static_cast<unsigned>(count),
            static_cast<unsigned>(bufferBytes));
    return false;  // the partial unique_ptrs free themselves
  }

  if (needBw) bwBuffer = std::move(bwNew);
  if (needPlanes) {
    lsbBuffer = std::move(lsbNew);
    msbBuffer = std::move(msbNew);
  }
  return true;
}
