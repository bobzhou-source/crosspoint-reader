#pragma once

// Host stand-in for FreeInkFont's FtFont (FreeType) exposing exactly the surface
// the current TtfEpdFont.cpp calls. It is deterministic and observable, so the
// vector FontCacheManager tests can assert cache lifetime in terms of real cost:
//   * rasterLoads                 - one FT_Load_Glyph each (a glyph MISS costs one,
//                                   a cache HIT costs none)
//   * kernQueries                 - one per distinct pair that reaches FreeType
//   * a streamed face calls its ReadFn on every glyph load, like a streamed .ttf
//     pulling glyf/loca bytes from the SD card, so tests can count stream reads
//   * bitmaps are a pure function of (glyph, face style); the first two pixels
//     fingerprint bold/italic so a glyph served from the wrong face is detectable
// Real FreeType/WenKai raster compatibility is NOT established here; it is covered
// by the real-backend WenKai probe and the emulator replay.

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace freeink {
namespace font {

struct GlyphBitmap {
  const uint8_t* pixels;
  uint16_t width;
  uint16_t height;
  int16_t xoff;
  int16_t yoff;
  int16_t advance;
};

struct HostFtState {
  size_t inits = 0;
  size_t deinits = 0;
  size_t rasterLoads = 0;  // one FT_Load_Glyph each: a glyph MISS costs one, a cache HIT none
  size_t kernQueries = 0;
  std::set<uint32_t> failLoad;    // the load fails: no advance, no bitmap
  std::set<uint32_t> failRaster;  // the load succeeds, the render fails: advance only
};
inline HostFtState g_ft;

class FtFont {
 public:
  using GlyphId = uint32_t;
  using ReadFn = unsigned long (*)(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);

  static constexpr uint32_t kUnmappedCodepoint = 0xE000;  // the cmap has no glyph here
  static constexpr uint32_t kSpace = 0x20;                // mapped, but draws nothing

  struct LineMetrics {
    int32_t ascender26_6 = 0;
    int32_t descender26_6 = 0;
    int32_t height26_6 = 0;
  };
  enum class HintingMode : uint8_t { Default = 0, None, Auto, Light, Native };
  struct RenderOptions {
    HintingMode hinting = HintingMode::Default;
    uint8_t interpreterVersion = 40;
    bool monochrome = false;
    bool stemDarkening = false;
    int32_t embolden26_6 = 0;
    int32_t slant16_16 = 0;
  };

  // Advance depends on size so a stale entry served after a resize is visible.
  static int32_t advance26_6For(const GlyphId glyph, const uint32_t size26_6) {
    return static_cast<int32_t>((size26_6 * (glyph % 7 + 3)) / 5 + glyph % 13);
  }

  bool init(const uint8_t*, uint32_t, uint16_t, int weight = 400, bool italic = false) {
    return begin(nullptr, nullptr, 0, weight, italic);
  }
  bool initStream(ReadFn read, void* ctx, unsigned long fileSize, uint16_t, int weight = 400, bool italic = false) {
    return begin(read, ctx, fileSize, weight, italic);
  }
  bool ready() const { return ready_; }
  bool setRenderOptions(const RenderOptions&) { return true; }
  void setGposByteBudget(size_t) {}
  void setGsubByteBudget(size_t) {}
  uint32_t ligatureGlyphId(const uint32_t*, unsigned) { return 0; }
  void releaseLigatureTable() {}
  void deinit() {
    if (ready_) ++g_ft.deinits;
    ready_ = false;
  }

  GlyphId glyphId(const uint32_t codepoint) const {
    if (!ready_ || codepoint == 0 || codepoint == kUnmappedCodepoint) return 0;
    return codepoint;
  }
  bool hasGlyph(const uint32_t codepoint) const { return glyphId(codepoint) != 0; }

  bool lineMetrics26_6(uint32_t, LineMetrics& out) {
    out.ascender26_6 = 24 * 64;
    out.descender26_6 = -8 * 64;
    out.height26_6 = 32 * 64;
    return true;
  }
  int16_t ascent(uint16_t) { return 24; }
  int16_t lineHeight(uint16_t) { return 32; }

  int32_t kerning26_6(const uint32_t left, const uint32_t right, uint32_t) {
    ++g_ft.kernQueries;
    return (static_cast<int32_t>((left * 7u + right * 13u) % 9u) - 4) * 8;
  }

  // The one glyph-load entry TtfEpdFont uses. `linearAdvance26_6` is written as soon
  // as the load succeeds, before the render step, exactly like the real FtFont, so a
  // render failure keeps its advance and a load failure leaves it untouched.
  const GlyphBitmap* rasterizeGlyph26_6(const GlyphId glyph, const uint32_t size26_6,
                                        int32_t* const linearAdvance26_6 = nullptr) {
    ++g_ft.rasterLoads;
    streamRead(glyph);
    if (!ready_ || glyph == 0 || g_ft.failLoad.count(glyph)) return nullptr;
    const int32_t advance = advance26_6For(glyph, size26_6);
    if (linearAdvance26_6) *linearAdvance26_6 = advance;
    if (g_ft.failRaster.count(glyph)) return nullptr;
    const uint16_t w = width(glyph);
    const uint16_t h = height(glyph);
    glyph_ = {nullptr, w, h, 1, static_cast<int16_t>(-static_cast<int>(h)), static_cast<int16_t>((advance + 32) >> 6)};
    if (w == 0 || h == 0) {
      glyph_.width = 0;
      glyph_.height = 0;
      return &glyph_;
    }
    bitmap_.assign(static_cast<size_t>(w) * h, 0);
    for (size_t i = 2; i < bitmap_.size(); ++i) bitmap_[i] = static_cast<uint8_t>((glyph * 37u + i * 29u) & 0xFFu);
    bitmap_[0] = weight_ >= 600 ? 255 : 0;  // style fingerprint
    bitmap_[1] = italic_ ? 255 : 0;
    glyph_.pixels = bitmap_.data();
    return &glyph_;
  }

 private:
  static uint16_t width(const GlyphId glyph) { return glyph == kSpace ? 0 : static_cast<uint16_t>(6 + glyph % 5); }
  static uint16_t height(const GlyphId glyph) { return glyph == kSpace ? 0 : static_cast<uint16_t>(8 + glyph % 4); }

  // A streamed face fetches the glyph's outline bytes from the file.
  void streamRead(const GlyphId glyph) {
    if (read_ == nullptr || fileSize_ <= sizeof(scratch_)) return;
    read_(ctx_, (static_cast<unsigned long>(glyph) * 64ul) % (fileSize_ - sizeof(scratch_)), scratch_, sizeof(scratch_));
  }

  bool begin(ReadFn read, void* ctx, unsigned long fileSize, const int weight, const bool italic) {
    read_ = read;
    ctx_ = ctx;
    fileSize_ = fileSize;
    weight_ = weight;
    italic_ = italic;
    ready_ = true;
    ++g_ft.inits;
    return true;
  }

  bool ready_ = false;
  int weight_ = 400;
  bool italic_ = false;
  ReadFn read_ = nullptr;
  void* ctx_ = nullptr;
  unsigned long fileSize_ = 0;
  unsigned char scratch_[64] = {};
  std::vector<uint8_t> bitmap_;
  GlyphBitmap glyph_{};
};

}  // namespace font
}  // namespace freeink
