#pragma once

#include <cstddef>
#include <cstdint>

// "Surprise me" candidate ranking. Kept free of storage and UI dependencies so
// the priority rules can be tested on the host: the caller classifies what it
// found on the card, this decides what to open.
namespace surprise {

enum class Origin : uint8_t { Inbox, Library };

constexpr int32_t NO_CONCEPT = INT32_MIN;
// Cosine 0.5: below this a concept "match" is noise and the roll decides.
constexpr int32_t CONCEPT_PREFER_MIN_DOT = 8065;

struct Candidate {
  const char* path = nullptr;
  Origin origin = Origin::Library;
  // Saved percentage, or -1 when the book has no saved progress at all.
  int progressPercent = -1;
  // True for short reads: articles and single-sitting items in the Inbox.
  bool shortRead = false;
  // Dot product of the item's inbox-concepts.idx vector with what was read
  // last (cosine x 127^2), or NO_CONCEPT when either is unknown.
  int32_t conceptDot = NO_CONCEPT;
};

// Rank of a candidate; lower opens first. Returns -1 when the item is not
// eligible (no path, or already finished). Finished books are never offered:
// the point is to start something new, not to reopen a last page.
int rank(const Candidate& candidate);

// Picks the path of the best candidate, or nullptr when nothing is eligible.
// Within the winning bucket a strong concept match wins (preferConceptMatch);
// otherwise the choice is `roll % bucketSize`, so the caller's random source
// stays outside the decision and the host can pin `roll`.
const char* pick(const Candidate* candidates, size_t count, uint32_t roll);

// The candidate of rank `bucket` with the highest conceptDot at or above
// CONCEPT_PREFER_MIN_DOT (first on ties), or nullptr when none reaches it.
const char* preferConceptMatch(const Candidate* candidates, size_t count, int bucket);

}  // namespace surprise
