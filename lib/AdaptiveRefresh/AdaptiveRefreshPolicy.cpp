#include "AdaptiveRefreshPolicy.h"

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

}  // namespace

void AdaptiveRefreshPolicy::reset(bool initialClearRequired) {
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

uint32_t AdaptiveRefreshPolicy::chargeFor(const Inputs& in) const {
  const auto& f = in.frame;
  uint64_t changed = f.historyValid ? f.changedPixels : estimateChangedPixels(f);
  if (changed > f.pixelCount) changed = f.pixelCount;
  uint64_t weight = kWeightUnitPermille;
  if (in.visibleGrayscale) weight += config_.weightGrayscalePermille;
  if (in.hasImage) weight += config_.weightImagePermille;
  if (in.visibleGrayscale && in.hasImage) weight += config_.weightGrayscaleImageExtraPermille;
  const uint64_t flat = static_cast<uint64_t>(f.pixelCount) * config_.frameChargePermille / 1000;
  return saturate(changed * weight / kWeightUnitPermille + flat);
}

AdaptiveRefreshPolicy::Decision AdaptiveRefreshPolicy::decide(const Inputs& in) {
  const auto& f = in.frame;
  pendingValid_ = true;
  pendingFrameValid_ = f.frameValid && f.pixelCount != 0;
  pendingInk_ = f.inkPixels;
  pendingCharge_ = pendingFrameValid_ ? chargeFor(in) : 0;

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
  } else if (static_cast<uint64_t>(debt_) + pendingCharge_ >=
             static_cast<uint64_t>(f.pixelCount) *
                 (in.invertedOutput ? config_.nightClearThresholdPermille : config_.clearThresholdPermille) / 1000) {
    reason = Reason::Debt;
  } else if (fastSinceClear_ >= config_.safetyCap) {
    reason = Reason::SafetyCap;
  }
  pendingRecord_ = labelFor(in, reason);
  return {reason == Reason::None ? Refresh::Fast : Refresh::Clear, reason};
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
  if (pixels) record.debtPermille = saturate((static_cast<uint64_t>(debt_) + pendingCharge_) * 1000 / pixels);
  record.thresholdPermille = in.invertedOutput ? config_.nightClearThresholdPermille : config_.clearThresholdPermille;
  record.inverted = in.invertedOutput;
  record.image = in.hasImage;
  record.grayscale = in.visibleGrayscale;
  return record;
}

void AdaptiveRefreshPolicy::commit(Refresh displayed) {
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
  } else {
    clearedCharge_ = 0;
    if (pendingValid_) {
      debt_ = saturate(static_cast<uint64_t>(debt_) + pendingCharge_);
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

void AdaptiveRefreshPolicy::imagePageShown() {
  if (config_.clearAfterImage) {
    requestClear(ClearSource::Image);
    return;
  }
  debt_ = saturate(static_cast<uint64_t>(debt_) + clearedCharge_);
  clearedCharge_ = 0;
}
