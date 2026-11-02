#pragma once

#include <Epub/FootnoteEntry.h>
#include <Epub/PageLink.h>
#include <HalMemory.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "PagePrerenderKey.h"

// Spare-PSRAM storage for the EPUB reader's idle prerender: one full-panel B/W
// body buffer and, when the grayscale route needs them, the two text-grayscale
// planes, plus the per-page metadata a presented page must republish
// (visible text offset, footnotes, links).
//
// The buffers are separate from the display framebuffer and are only ever used
// as GfxRenderer strip targets, so a build never touches what is on screen.
// Allocation is lazy, persistent for the activity's lifetime and refused unless
// a healthy PSRAM reserve stays free (page_prerender::psramAdmits); without
// PSRAM the class stays inert. Buffers are freed by the destructor, which the
// ActivityManager runs under the RenderLock, so the render task never sees a
// freed buffer.
class PagePrerender {
 public:
  // Takes the buffers the plan needs. `bufferBytes` is one whole-panel plane.
  // Returns false (nothing changed beyond freeing partial work) when PSRAM is
  // absent, below the reserve, or fragmented below one buffer.
  bool ensureBuffers(size_t bufferBytes, bool wantPlanes);

  uint8_t* bw() const { return bwBuffer.get(); }
  uint8_t* lsb() const { return lsbBuffer.get(); }
  uint8_t* msb() const { return msbBuffer.get(); }
  size_t bufferBytes() const { return bytes; }

  // Entry bookkeeping; see page_prerender::Slot.
  page_prerender::Slot slot;
  uint32_t visibleTextOffset = 0;
  std::vector<FootnoteEntry> footnotes;
  std::vector<PageLink> links;

  // Do not attempt another allocation before this millis() value after a
  // refusal, so a memory-starved device does not re-query PSRAM every page.
  unsigned long retryAllocAtMs = 0;

 private:
  HalMemory::PsramBuffer bwBuffer;
  HalMemory::PsramBuffer lsbBuffer;
  HalMemory::PsramBuffer msbBuffer;
  size_t bytes = 0;
};
