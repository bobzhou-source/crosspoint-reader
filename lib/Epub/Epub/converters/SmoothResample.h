#pragma once

#include <stdint.h>

// Smooth scaling and error diffusion for exact-size dithered decodes (the
// full-screen picture viewer and inline page pictures): a box filter when
// shrinking, centre-aligned bilinear when enlarging, then Sierra-2-4A error
// diffusion to the panel's four gray levels. Point sampling plus the 4x4 Bayer
// pattern left such pictures blocky and coarsely patterned.
namespace smooth_resample {

constexpr int FP_SHIFT = 16;
constexpr int32_t FP_ONE = 1 << FP_SHIFT;

// Source samples for output coordinate `d` along one axis; `inv` is
// source/output in 16.16 and samples are clamped to [lo, hi). Shrinking
// (inv >= 1): the box [first, last) the output pixel covers, weight < 0.
// Enlarging: the samples either side of the output pixel's centre, and the
// weight (0..256) of `last`.
struct Taps {
  int first;
  int last;
  int weight;
};

inline Taps taps(const int d, const int32_t inv, const int lo, const int hi) {
  Taps t;
  if (inv >= FP_ONE) {
    t.first = static_cast<int>((static_cast<int64_t>(d) * inv) >> FP_SHIFT);
    t.last = static_cast<int>((static_cast<int64_t>(d + 1) * inv) >> FP_SHIFT);
    t.weight = -1;
  } else {
    // (d + 0.5) * inv - 0.5, at least 0.
    int64_t centre = ((static_cast<int64_t>(2 * d + 1) * inv) >> 1) - (FP_ONE >> 1);
    if (centre < 0) centre = 0;
    t.first = static_cast<int>(centre >> FP_SHIFT);
    t.last = t.first + 1;
    t.weight = static_cast<int>((centre >> (FP_SHIFT - 8)) & 0xFF);
  }
  if (t.first < lo) t.first = lo;
  if (t.first > hi - 1) t.first = hi - 1;
  if (t.weight < 0) {
    if (t.last > hi) t.last = hi;
    if (t.last <= t.first) t.last = t.first + 1;
  } else if (t.last > hi - 1) {
    t.last = hi - 1;
  }
  return t;
}

// The filtered value of sample(i) over `t`.
template <typename Sample>
inline int filter(const Taps& t, Sample&& sample) {
  if (t.weight >= 0) return (sample(t.first) * (256 - t.weight) + sample(t.last) * t.weight) >> 8;
  int sum = 0;
  for (int i = t.first; i < t.last; i++) sum += sample(i);
  return sum / (t.last - t.first);
}

// Quantize `gray` (plus the error carried in) at output column x to a level
// 0..3 (black..white) and pass the error on Sierra-2-4A style: 2/4 to the
// right (`carry`), 1/4 down and 1/4 down-left (`down`, the next row's error
// by column, read and replaced here). A span decoded on its own (a JPEG
// block) starts with spanStart: down-left there would land in a column that
// span does not own, so it goes straight down. bleedQ8 scales the error
// passed on (256 = all of it, the plain filter).
inline uint8_t diffuse(const int gray, int16_t* down, const int x, const bool spanStart, int& carry,
                       const int bleedQ8 = 256) {
  const int value = gray + carry + down[x];
  int level = (value + 42) / 85;
  if (level < 0) level = 0;
  if (level > 3) level = 3;
  const int error = ((value - 85 * level) * bleedQ8) >> 8;
  const int quarter = error / 4;
  carry = error / 2;
  down[x] = static_cast<int16_t>(spanStart ? 2 * quarter : quarter);
  if (!spanStart) down[x - 1] = static_cast<int16_t>(down[x - 1] + quarter);
  return static_cast<uint8_t>(level);
}

// LiquidRefresh CfC v2 dither head. The reader installs a provider (its
// refresh policy's y1/y2 readouts) while it is open; without one every decode
// keeps gain 1.0 and full diffusion.
struct DitherParams {
  uint16_t contrastGainQ8 = 256;  // mid-gray S-curve opening, 256 = 1.0x
  uint16_t errorBleedQ8 = 256;    // share of the diffusion error passed on
};
using DitherProvider = DitherParams (*)(void* context, uint16_t edgePermille, uint16_t midGrayPermille);
inline DitherProvider ditherProvider = nullptr;
inline void* ditherProviderContext = nullptr;
inline void setDitherProvider(const DitherProvider provider, void* const context) {
  ditherProvider = provider;
  ditherProviderContext = context;
}
// Edge and mid-gray density of the last finished shaped decode (the first
// query of the next decode starts from them), permille of its pixels.
inline uint16_t lastEdgePermille = 60;
inline uint16_t lastMidGrayPermille = 400;

// Per-decode: counts strong horizontal transitions (|dGray| >= kEdgeStep)
// and mid-gray pixels (64..191) of the source grays, re-asks the provider
// every kRequeryPixels, and applies the contrast gain before diffusion.
class DitherShaper {
 public:
  static constexpr int kEdgeStep = 40;
  static constexpr uint32_t kRequeryPixels = 4096;

  DitherShaper() { query(lastEdgePermille, lastMidGrayPermille); }
  ~DitherShaper() {
    if (pixels_ >= kRequeryPixels) {
      lastEdgePermille = edgePermille();
      lastMidGrayPermille = midGrayPermille();
    }
  }
  DitherShaper(const DitherShaper&) = delete;
  DitherShaper& operator=(const DitherShaper&) = delete;

  // The gray to diffuse for source gray `gray`; spanStart: no left neighbour.
  int shape(const int gray, const bool spanStart) {
    if (!spanStart && prev_ >= 0 && (gray - prev_ >= kEdgeStep || prev_ - gray >= kEdgeStep)) ++edges_;
    prev_ = gray;
    if (gray >= 64 && gray <= 191) ++mid_;
    ++pixels_;
    if (++sinceQuery_ >= kRequeryPixels) query(edgePermille(), midGrayPermille());
    if (params_.contrastGainQ8 == 256) return gray;
    const int v = 128 + (((gray - 128) * static_cast<int>(params_.contrastGainQ8)) >> 8);
    return v < 0 ? 0 : v > 255 ? 255 : v;
  }
  int bleedQ8() const { return params_.errorBleedQ8; }
  const DitherParams& params() const { return params_; }
  uint16_t edgePermille() const { return pixels_ ? static_cast<uint16_t>(uint64_t{edges_} * 1000 / pixels_) : 0; }
  uint16_t midGrayPermille() const { return pixels_ ? static_cast<uint16_t>(uint64_t{mid_} * 1000 / pixels_) : 0; }

 private:
  void query(const uint16_t edge, const uint16_t mid) {
    sinceQuery_ = 0;
    if (ditherProvider) params_ = ditherProvider(ditherProviderContext, edge, mid);
  }

  DitherParams params_{};
  uint32_t pixels_ = 0;
  uint32_t edges_ = 0;
  uint32_t mid_ = 0;
  uint32_t sinceQuery_ = 0;
  int prev_ = -1;
};

}  // namespace smooth_resample
