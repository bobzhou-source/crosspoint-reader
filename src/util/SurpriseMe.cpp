#include "SurpriseMe.h"

#include <string_view>

namespace surprise {
namespace {
constexpr int RANK_UNREAD_INBOX = 0;
constexpr int RANK_SHORT_IN_PROGRESS_INBOX = 1;
constexpr int RANK_UNREAD_LIBRARY = 2;
constexpr int RANK_LONG_IN_PROGRESS_INBOX = 3;
constexpr int RANK_IN_PROGRESS_LIBRARY = 4;
}  // namespace

int rank(const Candidate& candidate) {
  if (!candidate.path || !*candidate.path) return -1;
  if (candidate.progressPercent >= 100) return -1;  // finished
  const bool unread = candidate.progressPercent <= 0;
  if (candidate.origin == Origin::Inbox) {
    if (unread) return RANK_UNREAD_INBOX;
    return candidate.shortRead ? RANK_SHORT_IN_PROGRESS_INBOX : RANK_LONG_IN_PROGRESS_INBOX;
  }
  return unread ? RANK_UNREAD_LIBRARY : RANK_IN_PROGRESS_LIBRARY;
}

const char* pick(const Candidate* candidates, const size_t count, const uint32_t roll) {
  if (!candidates || !count) return nullptr;
  int best = -1;
  size_t bucketSize = 0;
  for (size_t i = 0; i < count; ++i) {
    const int candidateRank = rank(candidates[i]);
    if (candidateRank < 0) continue;
    if (best < 0 || candidateRank < best) {
      best = candidateRank;
      bucketSize = 1;
    } else if (candidateRank == best) {
      ++bucketSize;
    }
  }
  if (best < 0) return nullptr;
  size_t target = roll % bucketSize;
  for (size_t i = 0; i < count; ++i) {
    if (rank(candidates[i]) != best) continue;
    if (target-- == 0) return candidates[i].path;
  }
  return nullptr;
}

}  // namespace surprise
