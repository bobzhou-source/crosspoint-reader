#pragma once

// Host seam for the SDK memory manager. TtfEpdFont::faultGlyph calls
// ensureFree() when a cache grow fails; on device that runs every registered
// eviction sink (GfxRenderer's gfx.ttfGlyphArenas calls releaseResidentCaches()
// on every TTF font). Here the test installs `onEvict` to run exactly that sink
// body, so the real eviction-lock behavior is exercised mid-fault.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace freeink {

enum class MemPool : uint8_t { Internal, Psram, Default };

class MemoryManager {
 public:
  static MemoryManager& instance() {
    static MemoryManager manager;
    return manager;
  }

  bool ensureFree(const size_t, const MemPool = MemPool::Default) {
    ensureFreeCalls++;
    if (onEvict) onEvict();
    return false;
  }

  std::function<void()> onEvict;
  size_t ensureFreeCalls = 0;
};

}  // namespace freeink
