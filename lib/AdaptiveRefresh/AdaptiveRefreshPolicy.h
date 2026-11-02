#pragma once

#include <FrameChangeMetrics.h>
#include <stdint.h>

// Content-aware "when does the next page need a clearing refresh" policy.
//
// Replaces a fixed page-count interval. Every displayed frame is charged an
// estimate of how many pixels the panel was asked to change, weighted by how
// hard that content is on the panel (images, visible grayscale). The charge
// accumulates as "ghost debt" that only a clearing refresh (HALF/FULL) erases.
// A page that changes little therefore lets many more fast refreshes through
// than a dense page or an image, but a conservative safety ceiling bounds the
// worst case. All constants are engineering estimates, NOT a calibration
// against physical ghosting, and there is no ghosting sensor behind them.
//
// Protocol (single owner, one frame at a time, no allocation, no I/O):
//   1. decide(inputs)   once per frame, before choosing the refresh mode. Pure
//                       with respect to debt: it only remembers this frame's
//                       charge so commit() can book it.
//   2. run the physical refresh with the returned mode (or whatever promotion
//      actually applies - manual/popup/inversion promotions are real clears).
//   3. commit(actual)   once, only if that refresh really reached the panel.
//                       Pass Clear whenever the hardware ran HALF or FULL.
//      Not committing (display failed/aborted) leaves all state untouched: the
//      panel still shows the previous frame.
// Frames whose refresh mode is dictated elsewhere (overlays, menus) that should
// still count are accounted the same way: decide(), ignore the answer, commit().
class AdaptiveRefreshPolicy {
 public:
  // Clear maps to the app's HALF refresh (FULL stays a manual/GfxRenderer promotion).
  enum class Refresh : uint8_t { Fast, Clear };

  enum class Reason : uint8_t {
    None,            // Fast: debt is within budget
    Debt,            // accumulated weighted change reached the budget
    Initial,         // nothing known about what the panel shows yet
    Requested,       // requestClear(): popup, post-image cleanup, buffer rebuild, ...
    External,        // a renderer-level clear promotion is already queued
    InvalidFrame,    // framebuffer unreadable or zero-sized panel metrics
    PolarityChange,  // output inversion flipped: the next refresh rewrites everything
    SafetyCap        // kSafetyCap fast refreshes in a row; bound only, not the normal rule
  };

  struct Inputs {
    freeink::FrameChangeMetrics frame;
    // Real page metadata (not a setting): the page contains a decoded image.
    bool hasImage = false;
    // A grayscale pass is actually planned for this frame AND the panel supports
    // it right now. Never set it just because AA is enabled or the panel is in
    // an inverted mode that renders BW only.
    bool visibleGrayscale = false;
    // A clearing refresh is already queued by the renderer (manual refresh,
    // popup close promotion); the frame is going to be a clear regardless.
    bool externalClearPending = false;
  };

  struct Decision {
    Refresh refresh;
    Reason reason;
  };

  // --- Identity: bump BOTH whenever a threshold, weight or rule below changes, so
  // benchmark/settings metadata can never describe a different policy than the one
  // that ran.
  static constexpr uint8_t kVersion = 1;
  static constexpr char kName[] = "adaptive-content-v1";

  // --- Tunables (public so docs and tests can cite them) -------------------
  // Clear when accumulated weighted change reaches this many thousandths of a
  // full screen of changed pixels (1500 = 1.5 screens).
  static constexpr uint32_t kClearThresholdPermille = 1500;
  // Hard ceiling on consecutive fast refreshes. Far above what normal reading
  // reaches (sparse pages need ~tens of turns); it only stops pathological
  // streams of tiny changes from never clearing.
  static constexpr uint16_t kSafetyCap = 60;
  // Charge multiplier in quarters (4 = 1.0x): a visible grayscale pass adds
  // 0.25x (AA text alone is not heavy), a page image adds 1.0x, and a grayscale
  // image adds a further 0.5x on top of both.
  static constexpr uint32_t kWeightUnit = 4;
  static constexpr uint32_t kWeightGrayscale = 1;
  static constexpr uint32_t kWeightImage = 4;
  static constexpr uint32_t kWeightGrayscaleImageExtra = 2;

  // initialClearRequired=false is the quick-resume case where the panel is
  // already known to show the restored page (allowFastInitialRefresh).
  explicit AdaptiveRefreshPolicy(bool initialClearRequired = true) { reset(initialClearRequired); }

  void reset(bool initialClearRequired = true);

  // Sticky until a Clear is committed. Not charged as debt.
  void requestClear() { clearRequested_ = true; }

  // The previous frame's ink is unknown (frames the policy never saw, buffer
  // lend/return). The fallback estimate then assumes the previous frame had the
  // current frame's density. Does not force a clear.
  void invalidateHistory() { prevInkKnown_ = false; }

  Decision decide(const Inputs& in);
  void commit(Refresh displayed);

  bool clearPending() const { return initialClear_ || clearRequested_; }
  uint32_t debtPixels() const { return debt_; }
  uint16_t fastSinceClear() const { return fastSinceClear_; }

  // Changed-pixel estimate used when the display has no exact last-displayed
  // baseline (single framebuffer without a valid async shadow, borrowed or
  // released secondary buffer, startup, inverted output). With P = pixelCount,
  // ink = logical-ink pixels, fg(x) = min(x, P - x) (the sparse side, so a
  // dark-background theme does not look "all ink"), and maj(x) = (2x > P):
  //   same background polarity:  min(P, fg(cur) + fg(prev))
  //   flipped polarity:          P - min(P, fg(cur) + fg(prev))
  // i.e. an upper bound that treats consecutive pages as non-overlapping, which
  // is accurate for page turns and conservative for everything else. prev is
  // the ink recorded at the last commit, or cur when unknown.
  uint32_t estimateChangedPixels(const freeink::FrameChangeMetrics& frame) const;

 private:
  uint32_t chargeFor(const Inputs& in) const;

  uint32_t debt_ = 0;            // weighted changed pixels since the last clear (saturating)
  uint32_t pendingCharge_ = 0;   // this frame's charge, set by decide()
  uint32_t pendingInk_ = 0;      // this frame's logical-ink pixels
  uint32_t prevInk_ = 0;         // ink of the last committed frame
  uint16_t fastSinceClear_ = 0;  // saturating
  bool initialClear_ = true;
  bool clearRequested_ = false;
  bool pendingValid_ = false;
  bool pendingFrameValid_ = false;
  bool prevInkKnown_ = false;
};
