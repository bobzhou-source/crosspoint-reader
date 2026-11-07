#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// Micro-checkpoints inside a long chapter.
//
// A 40-page chapter has no winnable goal; the reader only sees "12/40" ticking
// up. This turns the chapter into ~3-5 minute stretches: the next natural
// sub-section (a page that opens on a heading, a scene break or a horizontal
// rule), the next ~6-page interval when the chapter has no markers nearby, and
// the chapter end. The readers render the countdown only in its last few pages,
// where it is a goal rather than trivia.
//
// Pure arithmetic, no HAL or reader types: EpubReaderActivity feeds it section
// page indices, TxtReaderActivity feeds it page indices derived from the heading
// index, and both get the same answer for the same numbers.
namespace reading_pacing {

// Shorter chapters are a single sitting; a countdown would just add noise.
constexpr int kLongChapterPages = 6;
// Fallback cadence when a chapter has no markers: one micro-goal per six pages
// (~3-5 minutes at e-ink reading speed).
constexpr int kMicroBreakIntervalPages = 6;
// Only the last few pages of a stretch are worth counting down.
constexpr int kCountdownWindowPages = 3;

enum class CheckpointKind : uint8_t {
  None,
  MicroBreak,   // a sub-section, scene break or interval boundary inside the chapter
  ChapterEnd,   // the last page of the chapter
};

struct Checkpoint {
  CheckpointKind kind = CheckpointKind::None;
  int page = 0;      // zero-based page index the reader stops on
  int distance = 0;  // page turns left to reach it

  bool showsCountdown() const {
    return kind != CheckpointKind::None && distance >= 1 && distance <= kCountdownWindowPages;
  }
};

// `startPage` and `endPage` are inclusive, zero-based indices of the chapter on
// screen; `endPage - startPage + 1` is its length in pages.
constexpr bool isLongChapter(const int startPage, const int endPage) {
  return endPage >= startPage && (endPage - startPage + 1) > kLongChapterPages;
}

// Nearest checkpoint strictly after `currentPage` within [startPage, endPage].
// Candidates are the explicit `markerPages` (pages that open a sub-section), the
// ~6-page interval boundaries counted from the chapter start, and the chapter
// end; the closest one wins so a chapter with sparse markers still gets an
// interval micro-goal in between. A checkpoint the reader already stands on is
// never returned twice, and a finished chapter yields CheckpointKind::None.
inline Checkpoint nextCheckpoint(const int currentPage, const int startPage, const int endPage,
                                 const uint16_t* markerPages, const size_t markerCount) {
  Checkpoint result;
  if (endPage < startPage || currentPage < startPage) {
    return result;
  }

  int nearest = -1;
  for (size_t i = 0; i < markerCount; i++) {
    const int page = markerPages[i];
    // A marker on or behind the current page has been passed (or is where the
    // reader already is), and one at the chapter end is the chapter end.
    if (page <= currentPage || page >= endPage) {
      continue;
    }
    if (nearest < 0 || page < nearest) {
      nearest = page;
    }
  }

  for (int page = startPage + kMicroBreakIntervalPages; page < endPage; page += kMicroBreakIntervalPages) {
    if (page <= currentPage) {
      continue;
    }
    if (nearest < 0 || page < nearest) {
      nearest = page;
    }
    break;  // interval boundaries ascend, and the first one ahead is the nearest
  }

  if (nearest >= 0) {
    result.kind = CheckpointKind::MicroBreak;
    result.page = nearest;
    result.distance = nearest - currentPage;
    return result;
  }

  if (endPage > currentPage) {
    result.kind = CheckpointKind::ChapterEnd;
    result.page = endPage;
    result.distance = endPage - currentPage;
  }
  return result;
}

// "2p to break" inside the chapter, "1p left" at the chapter end. Writes nothing
// and returns 0 when no countdown is due.
inline size_t formatCountdown(const Checkpoint& checkpoint, char* out, const size_t outSize) {
  if (out == nullptr || outSize == 0 || !checkpoint.showsCountdown()) {
    return 0;
  }
  const char* suffix = checkpoint.kind == CheckpointKind::ChapterEnd ? "p left" : "p to break";
  const int written = std::snprintf(out, outSize, "%d%s", checkpoint.distance, suffix);
  if (written <= 0 || static_cast<size_t>(written) >= outSize) {
    return 0;
  }
  return static_cast<size_t>(written);
}

}  // namespace reading_pacing
