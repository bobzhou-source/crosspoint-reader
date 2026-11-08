#include "AdaptiveRefreshPolicy.h"

#include <string.h>

namespace {

constexpr uint32_t kMaxU32 = 0xFFFFFFFFu;

uint32_t saturate(uint64_t v) { return v > kMaxU32 ? kMaxU32 : static_cast<uint32_t>(v); }

// Sparse side of a frame: minority polarity pixels. Ink above P is a caller
// bug; clamp it rather than wrap.
uint64_t sparseSide(uint64_t ink, uint64_t pixels) {
  if (ink > pixels) ink = pixels;
  const uint64_t rest = pixels - ink;
  return ink < rest ? ink : rest;
}

bool inkMajority(uint64_t ink, uint64_t pixels) { return ink * 2 > pixels; }

// --- LiquidRefresh fixed point (32-bit only) --------------------------------
constexpr uint32_t kQ16 = 65536;
constexpr int32_t kRatioCap = 30000;  // readout features fit int16

constexpr int32_t clampI(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

// a * p / 1000 rounded down, exact, without 64-bit math (p <= 1000).
uint32_t permilleOf(uint32_t a, uint32_t p) { return a / 1000 * p + a % 1000 * p / 1000; }

// exp(-y/1000) in Q16 (0 beyond y = 16), table-free: halve x = y/1000 until
// x <= 0.5, take the Pade(2,2) rational (12 - 6x + x^2) / (12 + 6x + x^2),
// square back.
constexpr uint32_t expNegQ16(uint32_t yMilli) {
  if (yMilli >= 16000) return 0;
  uint32_t x = yMilli * 8389u >> 7;  // y/1000 in Q16 (65.536 ~ 8389/128)
  unsigned k = 0;
  while (x > kQ16 / 2) {
    x >>= 1;
    ++k;
  }
  const uint32_t x2 = x * x >> 16;
  uint32_t r = ((12 * kQ16 - 6 * x + x2) << 12) / ((12 * kQ16 + 6 * x + x2) >> 4);
  if (r > 0xFFFF) r = 0xFFFF;
  while (k--) r = r * r >> 16;
  return r;
}

// softplus(x/1000) in Q16: max(x, 0) + ln(1 + e^-|x|), with
// ln(1 + u) = 2 atanh(s), s = u / (2 + u) <= 1/3: 2 (s + s^3/3 + s^5/5).
// Only evaluated at compile time (kRateQ16).
constexpr uint32_t softplusQ16(int32_t xMilli) {
  const uint32_t u = expNegQ16(static_cast<uint32_t>(xMilli < 0 ? -xMilli : xMilli));
  const uint32_t s = (u << 16) / (2 * kQ16 + u);
  const uint32_t s2 = s * s >> 16;
  const uint32_t s3 = s * s2 >> 16;
  return (xMilli > 0 ? static_cast<uint32_t>(xMilli) * 8389u >> 7 : 0) + 2 * (s + s3 / 3 + (s3 * s2 >> 16) / 5);
}

// Relaxation rate softplus(f(polarity, class)) per second, Q16.
constexpr uint32_t rateQ16(bool inverted, int cls) {
  return softplusQ16(AdaptiveRefreshPolicy::kLiquidLogitMilli[cls] +
                     (inverted ? AdaptiveRefreshPolicy::kLiquidLogitInvertedMilli : 0));
}
constexpr uint16_t kRateQ16[2][3] = {{rateQ16(false, 0), rateQ16(false, 1), rateQ16(false, 2)},
                                     {rateQ16(true, 0), rateQ16(true, 1), rateQ16(true, 2)}};
// elapsedMs is clamped to kMaxDwellMs so elapsedMs * rate fits 32 bits; the
// slowest rate has relaxed to 0 permille well before it.
constexpr uint32_t kMaxDwellMs = 1u << 20;
static_assert(kRateQ16[0][0] < 4096 && expNegQ16(kMaxDwellMs * kRateQ16[1][2] >> 16) * 1000 < kQ16 / 2,
              "dwell clamp must not change rho");

int16_t addSat16(int32_t a, int32_t b) { return static_cast<int16_t>(clampI(a + b, -32768, 32767)); }

// LQRF v1 on disk (/.crosspoint/liquid-refresh.bin), little-endian.
struct LearnedFile {
  uint32_t magic;
  uint16_t version;
  uint16_t bytes;
  int16_t w[4];  // v0, v1, v2, b
  uint32_t updates;
  uint8_t reserved[8];
  uint32_t check;  // FNV-1a 32 of the 28 bytes before it
};
static_assert(sizeof(LearnedFile) == AdaptiveRefreshPolicy::kLearnedStateBytes, "LQRF layout");
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "LQRF is stored as the in-memory struct");

uint32_t fnv1a32(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  while (n--) h = (h ^ *p++) * 16777619u;
  return h;
}

}  // namespace

void AdaptiveRefreshPolicy::reset(bool initialClearRequired) {
  static_assert(sizeof(Liquid) + sizeof(LiquidReadout) <= 232, "LiquidRefresh RAM budget");
  debt_ = 0;
  pendingCharge_ = 0;
  pendingInk_ = 0;
  prevInk_ = 0;
  clearedCharge_ = 0;
  fastSinceClear_ = 0;
  initialClear_ = initialClearRequired;
  clearRequested_ = false;
  pendingValid_ = false;
  pendingFrameValid_ = false;
  prevInkKnown_ = false;
  pendingRecord_ = ClearRecord{};
  clearSource_ = ClearSource::None;
  imagePages_ = 0;
  grayPages_ = 0;
  imageRegion_ = Rect{};
  imageRegionUnknown_ = false;
  const uint32_t updates = liquid_.updates;
  liquid_ = Liquid{};
  liquid_.updates = updates;
  liquid_.retention = 1000;
  liquid_.tileGain = 1000;
}

uint32_t AdaptiveRefreshPolicy::estimateChangedPixels(const freeink::FrameChangeMetrics& frame) const {
  const uint64_t pixels = frame.pixelCount;
  const uint64_t curInk = frame.inkPixels;
  const uint64_t prevInk = prevInkKnown_ ? prevInk_ : curInk;
  const uint64_t sum = sparseSide(curInk, pixels) + sparseSide(prevInk, pixels);
  const uint64_t bounded = sum < pixels ? sum : pixels;
  const bool flipped = inkMajority(curInk > pixels ? pixels : curInk, pixels) !=
                       inkMajority(prevInk > pixels ? pixels : prevInk, pixels);
  return static_cast<uint32_t>(flipped ? pixels - bounded : bounded);
}

uint32_t AdaptiveRefreshPolicy::weightFor(const Inputs& in) const {
  uint64_t weight = kWeightUnitPermille;
  if (in.visibleGrayscale) weight += config_.weightGrayscalePermille;
  if (in.hasImage) weight += config_.weightImagePermille;
  if (in.visibleGrayscale && in.hasImage) weight += config_.weightGrayscaleImageExtraPermille;
  // Fast turn: the panel had no time to settle, charge up to the boost more.
  const uint32_t dt = elapsedFor(in);
  if (dt != 0 && dt < kFastTurnMs) {
    weight = weight * (1000 + kFastTurnBoostPermille * (kFastTurnMs - dt) / kFastTurnMs) / 1000;
  }
  return saturate(weight);
}

uint32_t AdaptiveRefreshPolicy::chargeFor(const Inputs& in) const {
  const auto& f = in.frame;
  uint64_t changed = f.historyValid ? f.changedPixels : estimateChangedPixels(f);
  if (changed > f.pixelCount) changed = f.pixelCount;
  const uint64_t flat = static_cast<uint64_t>(f.pixelCount) * config_.frameChargePermille / 1000;
  return saturate(changed * weightFor(in) / kWeightUnitPermille + flat);
}

uint16_t AdaptiveRefreshPolicy::liquidRhoPermille(uint32_t elapsedMs, const uint8_t cls, const bool inverted) {
  if (elapsedMs > kMaxDwellMs) elapsedMs = kMaxDwellMs;
  const uint32_t rate = kRateQ16[inverted][cls < 3 ? cls : 2];
  return static_cast<uint16_t>((expNegQ16(elapsedMs * rate >> 16) * 1000 + kQ16 / 2) >> 16);
}

int32_t AdaptiveRefreshPolicy::tileNext(uint8_t tile) const {
  const uint32_t v = static_cast<uint32_t>(liquid_.tile[tile]) * liquid_.retention / 1000 +
                     liquid_.staged[tile] * 4u * liquid_.tileGain / 1000;
  return v > 32767 ? 32767 : static_cast<int32_t>(v);
}

// Prepares this frame's liquid step and answers the debt rule. Pure with
// respect to committed state, like decide().
bool AdaptiveRefreshPolicy::liquidDebtDue(const Inputs& in) {
  const auto& f = in.frame;
  const uint32_t threshold = in.invertedOutput ? config_.nightClearThresholdPermille : config_.clearThresholdPermille;
  const uint32_t dt = elapsedFor(in);
  const uint32_t rho = liquidRhoPermille(dt, liquid_.residentClass, in.invertedOutput);
  const uint32_t floor = liquid_.debtFloor < debt_ ? liquid_.debtFloor : debt_;
  liquid_.pendingDebt = floor + permilleOf(debt_ - floor, rho);
  liquid_.retention = static_cast<uint16_t>(kLiquidFloorPermille + (1000 - kLiquidFloorPermille) * rho / 1000);
  const uint32_t gain = weightFor(in);
  liquid_.tileGain = static_cast<uint16_t>(gain > 0xFFFF ? 0xFFFF : gain);
  liquid_.pendingClass = in.hasImage ? kLiquidImage : in.visibleGrayscale ? kLiquidGray : kLiquidText;
  uint8_t streak = liquid_.fastStreak;
  if (dt >= kFastTurnMs) {
    streak = 0;
  } else if (dt != 0 && streak < kFastStreakFull) {
    ++streak;
  }
  liquid_.pendingStreak = streak;

  const uint64_t debt = static_cast<uint64_t>(liquid_.pendingDebt) + pendingCharge_;
  const uint64_t budget = static_cast<uint64_t>(f.pixelCount) * threshold / 1000;
  const bool tiles = liquid_.tilesActive || liquid_.tilesStaged;
  int16_t* const psi = liquid_.psi;
  uint32_t d = saturate(debt);
  uint32_t b = saturate(budget);
  while (d > kMaxU32 / 1000) {  // keep d * 1000 in 32 bits
    d >>= 1;
    b >>= 1;
  }
  psi[kReadoutDebt] = static_cast<int16_t>(d / 30 >= b ? kRatioCap : d * 1000 / b);
  psi[kReadoutPeak] = 0;
  if (tiles && threshold != 0) {
    int32_t peak = 0;
    int32_t sum = 0;
    for (uint8_t i = 0; i < kTileCount; ++i) {
      const int32_t v = tileNext(i);
      if (v > peak) peak = v;
      sum += v;
    }
    const uint32_t excess = static_cast<uint32_t>(peak - sum / kTileCount) * 1000 / (threshold * kTilePeakScale);
    psi[kReadoutPeak] = static_cast<int16_t>(excess > kRatioCap ? kRatioCap : excess);
  }
  psi[kReadoutStreak] = static_cast<int16_t>(streak * 1000 / kFastStreakFull);
  psi[kReadoutBias] = 1000;
  int32_t z = 0;
  for (int i = 0; i < 4; ++i) z += readout_.w[i] * psi[i];
  z /= 1000;
  liquid_.pressure = static_cast<int16_t>(clampI(z, 0, 1000));
  // Default readout, no tiles, no streak: z >= 1000 is exactly debt >= budget;
  // compare the integers themselves so the content-v2 rule is bit-exact.
  if (!tiles && streak == 0 && readout_ == LiquidReadout{}) return debt >= budget;
  return z >= 1000;
}

AdaptiveRefreshPolicy::Decision AdaptiveRefreshPolicy::decide(const Inputs& in) {
  const auto& f = in.frame;
  pendingValid_ = true;
  pendingFrameValid_ = f.frameValid && f.pixelCount != 0;
  pendingInk_ = f.inkPixels;
  pendingCharge_ = pendingFrameValid_ ? chargeFor(in) : 0;
  const bool debtDue = liquidDebtDue(in);

  Reason reason = Reason::None;
  if (initialClear_) {
    reason = Reason::Initial;
  } else if (clearRequested_) {
    reason = Reason::Requested;
  } else if (in.externalClearPending) {
    reason = Reason::External;
  } else if (!pendingFrameValid_) {
    reason = Reason::InvalidFrame;
  } else if (f.polarityChangePending) {
    reason = Reason::PolarityChange;
  } else if (debtDue) {
    reason = Reason::Debt;
  } else if (fastSinceClear_ >= config_.safetyCap) {
    reason = Reason::SafetyCap;
  }
  pendingRecord_ = labelFor(in, reason);
  if (reason == Reason::Requested && clearSource_ == ClearSource::Image && config_.imageClearRegion) {
    Rect region{};
    pendingRecord_.regionFallback = regionFor(in, debtDue, region);
    if (pendingRecord_.regionFallback == RegionFallback::None) {
      pendingRecord_.regionOnly = true;
      pendingRecord_.region = region;
      return {Refresh::Region, reason, region};
    }
  }
  return {reason == Reason::None ? Refresh::Fast : Refresh::Clear, reason};
}

AdaptiveRefreshPolicy::RegionFallback AdaptiveRefreshPolicy::regionFor(const Inputs& in, const bool debtDue,
                                                                       Rect& region) const {
  const auto& f = in.frame;
  // The rest of decide()'s chain: any of these needs the whole screen.
  if (in.externalClearPending || !pendingFrameValid_ || f.polarityChangePending || debtDue ||
      fastSinceClear_ >= config_.safetyCap) {
    return RegionFallback::Overruled;
  }
  if (imageRegionUnknown_) return RegionFallback::Unknown;
  const int32_t x0 = imageRegion_.x > 0 ? imageRegion_.x : 0;
  const int32_t y0 = imageRegion_.y > 0 ? imageRegion_.y : 0;
  const int32_t right = static_cast<int32_t>(imageRegion_.x) + imageRegion_.w;
  const int32_t bottom = static_cast<int32_t>(imageRegion_.y) + imageRegion_.h;
  const int32_t x1 = right < in.screenWidth ? right : in.screenWidth;
  const int32_t y1 = bottom < in.screenHeight ? bottom : in.screenHeight;
  if (imageRegion_.empty() || x1 <= x0 || y1 <= y0) return RegionFallback::OffScreen;
  const uint64_t area = static_cast<uint64_t>(x1 - x0) * static_cast<uint64_t>(y1 - y0);
  const uint64_t screen = static_cast<uint64_t>(in.screenWidth) * in.screenHeight;
  if (area * 1000 > screen * kRegionMaxPermille) return RegionFallback::TooLarge;
  if (!in.regionClearAvailable) return RegionFallback::Unavailable;
  region = {static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0),
            static_cast<int16_t>(y1 - y0)};
  return RegionFallback::None;
}

AdaptiveRefreshPolicy::ClearRecord AdaptiveRefreshPolicy::labelFor(const Inputs& in, const Reason reason) const {
  ClearRecord record;
  record.reason = reason;
  if (reason == Reason::Requested) {
    record.source = clearSource_ == ClearSource::None ? ClearSource::Other : clearSource_;
  } else if (reason == Reason::External) {
    record.source = ClearSource::External;
  }
  if ((reason == Reason::Requested || reason == Reason::External) && in.externalClearPending && in.externalManual) {
    record.source = ClearSource::Manual;
  }
  const uint64_t pixels = pendingFrameValid_ ? in.frame.pixelCount : 0;
  if (pixels) {
    record.debtPermille = saturate((static_cast<uint64_t>(liquid_.pendingDebt) + pendingCharge_) * 1000 / pixels);
  }
  record.thresholdPermille = in.invertedOutput ? config_.nightClearThresholdPermille : config_.clearThresholdPermille;
  record.inverted = in.invertedOutput;
  record.image = in.hasImage;
  record.grayscale = in.visibleGrayscale;
  return record;
}

void AdaptiveRefreshPolicy::commit(Refresh displayed) {
  liquidCommit(displayed);
  if (displayed == Refresh::Clear) {
    if (observer_) {
      ClearRecord record = pendingValid_ ? pendingRecord_ : ClearRecord{};
      if (!pendingValid_) record.source = clearSource_;
      record.fastTurns = fastSinceClear_;
      record.imagePages = imagePages_;
      record.grayPages = grayPages_;
      observer_(observerContext_, record);
    }
    debt_ = 0;
    fastSinceClear_ = 0;
    initialClear_ = false;
    clearRequested_ = false;
    clearedCharge_ = pendingValid_ ? pendingCharge_ : 0;
    clearSource_ = ClearSource::None;
    imagePages_ = 0;
    grayPages_ = 0;
    imageRegion_ = Rect{};
    imageRegionUnknown_ = false;
  } else {
    if (displayed == Refresh::Region) {
      // Ends the picture clear only; the frame itself was drawn fast and the
      // debt window carries on.
      if (observer_) {
        ClearRecord record = pendingValid_ ? pendingRecord_ : ClearRecord{};
        if (!pendingValid_) record.source = clearSource_;
        record.regionOnly = true;
        record.fastTurns = fastSinceClear_;
        record.imagePages = imagePages_;
        record.grayPages = grayPages_;
        observer_(observerContext_, record);
      }
      clearRequested_ = false;
      clearSource_ = ClearSource::None;
      imageRegion_ = Rect{};
      imageRegionUnknown_ = false;
    }
    clearedCharge_ = 0;
    if (pendingValid_) {
      debt_ = saturate(static_cast<uint64_t>(liquid_.pendingDebt) + pendingCharge_);
      if (fastSinceClear_ != 0xFFFFu) ++fastSinceClear_;
      if (pendingRecord_.image && imagePages_ != 0xFFFFu) ++imagePages_;
      if (pendingRecord_.grayscale && grayPages_ != 0xFFFFu) ++grayPages_;
    }
  }
  if (pendingValid_ && pendingFrameValid_) {
    prevInk_ = pendingInk_;
    prevInkKnown_ = true;
  } else {
    prevInkKnown_ = false;
  }
  pendingValid_ = false;
}

// Runs before commit() books the debt: reads the pending decide() state.
void AdaptiveRefreshPolicy::liquidCommit(const Refresh displayed) {
  if (pendingValid_) {
    liquid_.fastStreak = liquid_.pendingStreak;
    if (pendingFrameValid_) liquid_.residentClass = liquid_.pendingClass;
  }
  if (displayed == Refresh::Clear) {
    for (auto& t : liquid_.tile) t = 0;
    liquid_.tilesActive = false;
    liquid_.debtFloor = 0;
    // A manual clear keeps the trace for recordManualRefresh(); any other
    // clear ends the episode without a complaint.
    if (!(pendingValid_ && pendingRecord_.manual())) {
      for (auto& e : liquid_.trace) e = 0;
      liquid_.traceTurns = 0;
    }
  } else if (pendingValid_) {
    const uint32_t debt = saturate(static_cast<uint64_t>(liquid_.pendingDebt) + pendingCharge_);
    const uint32_t floor =
        saturate(static_cast<uint64_t>(liquid_.debtFloor) + permilleOf(pendingCharge_, kLiquidFloorPermille));
    liquid_.debtFloor = floor < debt ? floor : debt;
    if (liquid_.tilesActive || liquid_.tilesStaged) {
      for (uint8_t i = 0; i < kTileCount; ++i) liquid_.tile[i] = static_cast<int16_t>(tileNext(i));
      liquid_.tilesActive = true;
    }
    if (pendingFrameValid_) {
      // e = gamma*lambda*e + (a - p)*psi with a = 0: the policy chose fast.
      for (int i = 0; i < 4; ++i) {
        liquid_.trace[i] =
            addSat16(liquid_.trace[i] * kTraceDecayPermille / 1000, -liquid_.pressure * liquid_.psi[i] / 1000);
      }
      if (liquid_.traceTurns != 0xFF) ++liquid_.traceTurns;
    }
  }
  for (auto& s : liquid_.staged) s = 0;
  liquid_.tilesStaged = false;
  liquid_.stagedElapsedMs = 0;
}

void AdaptiveRefreshPolicy::recordTileTransitions(const uint16_t tileChanged[kTileCount], const uint32_t elapsedMs) {
  for (uint8_t i = 0; i < kTileCount; ++i) {
    const uint16_t v = tileChanged[i] > 1000 ? 1000 : tileChanged[i];
    liquid_.staged[i] = static_cast<uint8_t>((v + 2) / 4);
  }
  liquid_.tilesStaged = true;
  liquid_.stagedElapsedMs = elapsedMs;
}

bool AdaptiveRefreshPolicy::recordManualRefresh() {
  if (liquid_.traceTurns == 0) return false;
  // m = 1: v -= eta * m * e. The trace holds -p*psi, so every feature active
  // before the complaint gains weight and the clear comes earlier.
  for (int i = 0; i < 4; ++i) {
    readout_.w[i] = addSat16(readout_.w[i], -liquid_.trace[i] * kLearnRatePermille / 1000);
    liquid_.trace[i] = 0;
  }
  liquid_.traceTurns = 0;
  clampReadout();
  if (liquid_.updates != kMaxU32) ++liquid_.updates;
  return true;
}

// Box bounds, then keep the plain debt threshold (1000 - b - v2*streak) / v0
// within [500, 1500] permille of the configured one for any streak.
void AdaptiveRefreshPolicy::clampReadout() {
  static constexpr int16_t kLo[4] = {667, 500, 0, -500};
  static constexpr int16_t kHi[4] = {2000, 2000, 500, 500};
  int16_t* const w = readout_.w;
  for (int i = 0; i < 4; ++i) w[i] = static_cast<int16_t>(clampI(w[i], kLo[i], kHi[i]));
  const int32_t lowest = 1000 - w[kReadoutStreak] - (w[kReadoutDebt] + 1) / 2;  // threshold >= 50%
  const int32_t highest = 1000 - w[kReadoutDebt] * 3 / 2;                        // threshold <= 150%
  w[kReadoutBias] = static_cast<int16_t>(clampI(w[kReadoutBias], highest, lowest));
}

uint32_t AdaptiveRefreshPolicy::learnedThresholdPermille(const bool inverted) const {
  const uint32_t threshold = inverted ? config_.nightClearThresholdPermille : config_.clearThresholdPermille;
  const uint64_t scaled = static_cast<uint64_t>(threshold) * static_cast<uint32_t>(1000 - readout_.w[kReadoutBias]);
  return static_cast<uint32_t>(scaled / static_cast<uint32_t>(readout_.w[kReadoutDebt]));
}

void AdaptiveRefreshPolicy::resetLearnedState() {
  readout_ = LiquidReadout{};
  liquid_.updates = 0;
}

size_t AdaptiveRefreshPolicy::saveLearnedState(uint8_t* buf, const size_t len) const {
  if (!buf || len < kLearnedStateBytes) return 0;
  LearnedFile file{};
  file.magic = kLearnedStateMagic;
  file.version = kLearnedStateVersion;
  file.bytes = kLearnedStateBytes;
  memcpy(file.w, readout_.w, sizeof file.w);
  file.updates = liquid_.updates;
  file.check = fnv1a32(reinterpret_cast<const uint8_t*>(&file), offsetof(LearnedFile, check));
  memcpy(buf, &file, sizeof file);
  return kLearnedStateBytes;
}

bool AdaptiveRefreshPolicy::loadLearnedState(const uint8_t* buf, const size_t len) {
  LearnedFile file;
  if (!buf || len < kLearnedStateBytes) return false;
  memcpy(&file, buf, sizeof file);
  if (file.magic != kLearnedStateMagic || file.version != kLearnedStateVersion || file.bytes != kLearnedStateBytes ||
      file.check != fnv1a32(buf, offsetof(LearnedFile, check))) {
    return false;
  }
  memcpy(readout_.w, file.w, sizeof file.w);
  liquid_.updates = file.updates;
  clampReadout();
  return true;
}

void AdaptiveRefreshPolicy::imagePageShown(const Rect pictures) {
  if (config_.clearAfterImage) {
    if (pictures.empty()) {
      imageRegionUnknown_ = true;
    } else if (imageRegion_.empty()) {
      imageRegion_ = pictures;
    } else {
      const int32_t x0 = imageRegion_.x < pictures.x ? imageRegion_.x : pictures.x;
      const int32_t y0 = imageRegion_.y < pictures.y ? imageRegion_.y : pictures.y;
      const int32_t x1 = imageRegion_.x + imageRegion_.w > pictures.x + pictures.w ? imageRegion_.x + imageRegion_.w
                                                                                   : pictures.x + pictures.w;
      const int32_t y1 = imageRegion_.y + imageRegion_.h > pictures.y + pictures.h ? imageRegion_.y + imageRegion_.h
                                                                                   : pictures.y + pictures.h;
      imageRegion_ = {static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0),
                      static_cast<int16_t>(y1 - y0)};
    }
    requestClear(ClearSource::Image);
    return;
  }
  debt_ = saturate(static_cast<uint64_t>(debt_) + clearedCharge_);
  liquid_.debtFloor =
      saturate(static_cast<uint64_t>(liquid_.debtFloor) + permilleOf(clearedCharge_, kLiquidFloorPermille));
  clearedCharge_ = 0;
}
