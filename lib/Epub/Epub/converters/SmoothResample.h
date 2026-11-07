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
// span does not own, so it goes straight down.
inline uint8_t diffuse(const int gray, int16_t* down, const int x, const bool spanStart, int& carry) {
  const int value = gray + carry + down[x];
  int level = (value + 42) / 85;
  if (level < 0) level = 0;
  if (level > 3) level = 3;
  const int error = value - 85 * level;
  const int quarter = error / 4;
  carry = error / 2;
  down[x] = static_cast<int16_t>(spanStart ? 2 * quarter : quarter);
  if (!spanStart) down[x - 1] = static_cast<int16_t>(down[x - 1] + quarter);
  return static_cast<uint8_t>(level);
}

}  // namespace smooth_resample
