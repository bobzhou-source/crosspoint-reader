#pragma once

#include <Epub/ReaderRenderSpec.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

// Pure decision logic for the EPUB reader's idle page prerender: which page is
// worth prerendering, whether a stored page body may stand in for a fresh
// render, and when spare PSRAM may be taken. Hardware-free so the invalidation
// rules are unit-tested on the host (test/epub_prerender_key).
//
// A prerendered entry holds the page BODY only (text, no status bar): the
// status bar carries volatile data (battery, clock, bookmark, progress) and is
// always drawn fresh at present time, so it can never be stale.
namespace page_prerender {

// What an entry's buffers hold. Computed by planFor() from the same inputs when
// the entry is built and when it is presented, so a plan mismatch is a miss.
enum class Plan : uint8_t {
  None = 0,         // not prerenderable: not committed, never matches
  BwOnly = 1,       // B/W body only; the page needs no text-grayscale planes
  BwAndPlanes = 2,  // B/W body plus the LSB and MSB text-grayscale planes
};

struct PlanInputs {
  bool pageHasImages = false;           // image decode/polarity work is not prerendered
  bool needsAnyGrayscale = false;       // any grayscale pass follows the B/W pass
  bool needsTextGrayscale = false;      // anti-aliased text planes are wanted
  bool tiledGrayscale = false;          // strip-upload grayscale route
  bool overlapRefresh = false;          // planes are buffered while the B/W base refreshes
  bool absoluteImageGrayscale = false;  // image-page grayscale mode
};

constexpr Plan planFor(const PlanInputs& in) {
  if (in.pageHasImages || in.absoluteImageGrayscale) return Plan::None;
  if (!in.needsAnyGrayscale) return Plan::BwOnly;
  // Only the buffered tiled route (the one the presenter already runs with
  // whole-plane buffers) can consume prerendered planes.
  if (in.needsTextGrayscale && in.tiledGrayscale && in.overlapRefresh) return Plan::BwAndPlanes;
  return Plan::None;
}

// Everything a prerendered body depends on besides the page's own content.
// Equal keys mean the stored pixels are what a fresh render would produce.
struct Key {
  uint32_t bookId = 0;            // identity of the open book
  int32_t spineIndex = -1;        // section within the book
  int32_t pageIndex = -1;         // page within the section
  uint32_t sectionPageCount = 0;  // page count of the section the page was loaded from
  ReaderRenderSpec spec;          // layout spec the section was built with (incl. font id, viewport)
  int32_t marginTop = 0;          // oriented page origin and margins, status-bar and
  int32_t marginRight = 0;        // automatic-turn adjustments included
  int32_t marginBottom = 0;
  int32_t marginLeft = 0;
  uint8_t orientation = 0;
  bool screenInverted = false;
  bool textAntiAliasing = false;
  int32_t renderFontId = 0;  // font id the glyphs are drawn with (SETTINGS.getReaderFontId() at render time)
  Plan plan = Plan::None;

  bool operator==(const Key& o) const {
    return bookId == o.bookId && spineIndex == o.spineIndex && pageIndex == o.pageIndex &&
           sectionPageCount == o.sectionPageCount && specEquals(spec, o.spec) && marginTop == o.marginTop &&
           marginRight == o.marginRight && marginBottom == o.marginBottom && marginLeft == o.marginLeft &&
           orientation == o.orientation && screenInverted == o.screenInverted &&
           textAntiAliasing == o.textAntiAliasing && renderFontId == o.renderFontId && plan == o.plan;
  }
  bool operator!=(const Key& o) const { return !(*this == o); }

  static bool specEquals(const ReaderRenderSpec& a, const ReaderRenderSpec& b) {
    return a.fontId == b.fontId && a.lineCompression == b.lineCompression &&
           a.extraParagraphSpacing == b.extraParagraphSpacing && a.characterSpacing == b.characterSpacing &&
           a.wordSpacingPercent == b.wordSpacingPercent && a.paragraphAlignment == b.paragraphAlignment &&
           a.viewportWidth == b.viewportWidth && a.viewportHeight == b.viewportHeight &&
           a.hyphenationEnabled == b.hyphenationEnabled && a.embeddedStyle == b.embeddedStyle &&
           a.imageRendering == b.imageRendering && a.focusReadingEnabled == b.focusReadingEnabled;
  }
};

// FNV-1a, for the book identity carried in a Key.
inline uint32_t hashBytes(const char* data, size_t len, uint32_t seed = 2166136261u) {
  uint32_t h = seed;
  for (size_t i = 0; i < len; ++i) {
    h ^= static_cast<uint8_t>(data[i]);
    h *= 16777619u;
  }
  return h;
}

// The single stored entry. matches() is the only way a stored body may be
// presented: it needs a committed entry, no invalidation since, and an equal key.
class Slot {
 public:
  // Refuses keys whose plan is None: those pages are never prerendered.
  bool commit(const Key& key) {
    if (key.plan == Plan::None || key.spineIndex < 0 || key.pageIndex < 0) {
      valid_.store(false, std::memory_order_relaxed);
      return false;
    }
    key_ = key;
    valid_.store(true, std::memory_order_release);
    return true;
  }
  // Any input change, overlay, settings/orientation/font change, section
  // rebuild or abandoned build ends the entry's life.
  void invalidate() { valid_.store(false, std::memory_order_release); }
  bool valid() const { return valid_.load(std::memory_order_acquire); }
  bool matches(const Key& wanted) const { return valid() && key_ == wanted; }
  const Key& key() const { return key_; }

 private:
  Key key_;
  std::atomic<bool> valid_{false};
};

// Where the page after the one on screen lives.
struct NextPage {
  enum class Kind : uint8_t {
    None,         // end of the book (or nothing known)
    SameSection,  // page + 1 of the loaded section
    NextSection,  // page 0 of spine + 1; prerendered only from a complete cache
  };
  Kind kind = Kind::None;
  int spineIndex = -1;
  int pageIndex = -1;
};

constexpr NextPage nextPageAfter(const int spineIndex, const int pageIndex, const int sectionPageCount,
                                 const int spineCount) {
  if (spineIndex < 0 || pageIndex < 0 || sectionPageCount <= 0 || pageIndex >= sectionPageCount) return {};
  if (pageIndex + 1 < sectionPageCount) return {NextPage::Kind::SameSection, spineIndex, pageIndex + 1};
  if (spineIndex + 1 < spineCount) return {NextPage::Kind::NextSection, spineIndex + 1, 0};
  return {};
}

// Spare PSRAM may be taken only while a healthy reserve stays free, and only
// as buffers that fit the largest free block.
constexpr size_t kPsramReserveBytes = 1024 * 1024;

constexpr bool psramAdmits(const size_t freeBytes, const size_t largestBlockBytes, const size_t bufferBytes,
                           const size_t bufferCount, const size_t reserveBytes = kPsramReserveBytes) {
  if (bufferBytes == 0 || bufferCount == 0) return false;
  return largestBlockBytes >= bufferBytes && freeBytes >= bufferBytes * bufferCount + reserveBytes;
}

// Logged in benchmark `epub_phase` rows of phase Prerender (detail slot).
enum class Outcome : uint8_t {
  Built = 1,             // B/W body stored
  BuiltWithPlanes = 2,   // B/W body plus both grayscale planes stored
  Abandoned = 3,         // input arrived mid-work; entry discarded
  SkippedMemory = 4,     // not enough spare PSRAM (or allocation failed)
  SkippedIneligible = 5  // page not prerenderable, or its section not cached/complete
};

}  // namespace page_prerender
