#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// "Substack boomerang": a quick break from a real book into one unread item from
// the Inbox drop folder, which returns to the book when the item ends.
//
// Pure decisions only -- which path counts as the Inbox, and which unread item to
// pick -- so the selection rules are host-testable; the directory scan and the
// activity swap live in BoomerangInbox.cpp and ReaderActivity.
namespace boomerang {

constexpr const char* kInboxFolder = "/books/Inbox";

// How a reader menu should draw its Quick Break row. Both reader menus take this,
// so the two formats cannot disagree about when a break is offered.
struct Row {
  bool quickBreak = false;     // "Quick Break": the Inbox holds an unread item
  std::string backToTitle;     // "Back to <title>": a break this reader opened is open
};

// Lower-cases one ASCII byte for path comparisons. FAT is case-insensitive, so
// "/books/INBOX/novel.epub" must classify like "/books/Inbox/novel.epub".
constexpr char foldAscii(const char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool equalsIgnoreCase(const std::string_view a, const std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); i++) {
    if (foldAscii(a[i]) != foldAscii(b[i])) {
      return false;
    }
  }
  return true;
}

// Case-insensitive ordinal comparison, used to pick a deterministic break item.
// Not natural sort: the pick only has to be stable, not alphabetical-looking.
constexpr int compareIgnoreCase(const std::string_view a, const std::string_view b) {
  const size_t shared = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = 0; i < shared; i++) {
    const char ca = foldAscii(a[i]);
    const char cb = foldAscii(b[i]);
    if (ca != cb) {
      return ca < cb ? -1 : 1;
    }
  }
  if (a.size() == b.size()) {
    return 0;
  }
  return a.size() < b.size() ? -1 : 1;
}

// True when the book sits directly in (or below) the Inbox folder. Matching is by
// whole path component, so "/books/Inbox2/x.epub" and "/Bookshelf/Inbox/x.epub"
// are not Inbox books.
inline bool isInboxBookPath(const std::string_view bookPath) {
  constexpr std::string_view kFolder = kInboxFolder;  // "/books/Inbox", no trailing slash
  size_t at = 0;
  while (at < bookPath.size() && (bookPath[at] == '/' || bookPath[at] == '\\')) {
    at++;
  }
  const size_t needed = kFolder.size() - 1;  // "books/Inbox"
  if (bookPath.size() - at < needed) {
    return false;
  }
  // Whole-component match, so a sibling such as "/books/Inbox-old" or
  // "/bookshelf/Inbox" does not classify as the Inbox.
  for (size_t i = 0; i < needed; i++) {
    const char expected = kFolder[i + 1];
    const char actual = bookPath[at + i];
    if (expected == '/') {
      if (actual != '/' && actual != '\\') {
        return false;
      }
    } else if (actual == '/' || actual == '\\' || foldAscii(actual) != foldAscii(expected)) {
      return false;
    }
  }
  const size_t after = at + needed;
  return after == bookPath.size() || bookPath[after] == '/' || bookPath[after] == '\\';
}

// One scanned Inbox file and whether it already has saved progress. A finished
// item keeps its progress file, so it is "started" too and never re-picked.
struct Candidate {
  std::string_view path;
  bool started = false;
};

// True when `candidate` should replace `current` as the break item: only unread
// items qualify, and the lowest path wins so repeated Quick Breaks are stable.
inline bool prefersBreakCandidate(const std::string_view current, const std::string_view candidate,
                                  const bool candidateStarted) {
  if (candidateStarted) {
    return false;
  }
  if (current.empty()) {
    return true;
  }
  return compareIgnoreCase(candidate, current) < 0;
}

// Index of the unread candidate with the lowest path, or `count` when every
// candidate has been started (no break available).
inline size_t selectBreakCandidate(const Candidate* candidates, const size_t count) {
  size_t best = count;
  for (size_t i = 0; i < count; i++) {
    if (candidates[i].started) {
      continue;
    }
    if (best == count || compareIgnoreCase(candidates[i].path, candidates[best].path) < 0) {
      best = i;
    }
  }
  return best;
}

// Filename without its folder and extension, for "Back to <title>" when the
// reader was opened without a loaded title (a book that failed to parse).
inline std::string_view displayName(const std::string_view path) {
  std::string_view name = path;
  const size_t slash = name.find_last_of("/\\");
  if (slash != std::string_view::npos) {
    name.remove_prefix(slash + 1);
  }
  const size_t dot = name.find_last_of('.');
  if (dot != std::string_view::npos && dot > 0) {
    name = name.substr(0, dot);
  }
  return name;
}

// Which Quick Break row a reader menu should offer for the book on screen.
enum class BreakRowKind : uint8_t {
  None,
  QuickBreak,  // the way out: user pressed the row, so the reader leaves the book for a break
  BackToBook,  // the way back: a break is open and it is not onto this very book
};

// `breakPath` is the recorded return target (empty when no break is open),
// `hasInboxItem` whether an unread Inbox item exists. A break never stacks: while
// one is open the only row is the way back.
constexpr BreakRowKind planBreakRow(const std::string_view bookPath, const std::string_view breakPath,
                                     const bool isInbox, const bool hasInboxItem) {
  if (!breakPath.empty()) {
    return breakPath == bookPath ? BreakRowKind::None : BreakRowKind::BackToBook;
  }
  if (isInbox || bookPath.empty() || !hasInboxItem) {
    return BreakRowKind::None;
  }
  return BreakRowKind::QuickBreak;
}

// What ending a break should do, resolved from the recorded target alone.
enum class ReturnPlan : uint8_t {
  Nothing,     // no break open
  BookGone,    // the target vanished: clear the break and stay put
  SelfBook,    // the target is the book on screen: clear the break and stay put
  BackToBook,  // navigate back to the recorded target
};

constexpr ReturnPlan planReturn(const std::string_view bookPath, const std::string_view target,
                                const bool targetExists) {
  if (target.empty()) {
    return ReturnPlan::Nothing;
  }
  if (target == bookPath) {
    return ReturnPlan::SelfBook;
  }
  return targetExists ? ReturnPlan::BackToBook : ReturnPlan::BookGone;
}

// Title for the "Back to <title>" row: the stored title, else the file name; empty
// when there is nothing to go back to (no break, or this is the source book).
inline std::string_view returnLabel(const std::string_view bookPath, const std::string_view target,
                                    const std::string_view storedTitle) {
  if (target.empty() || target == bookPath) {
    return {};
  }
  return storedTitle.empty() ? displayName(target) : storedTitle;
}

}  // namespace boomerang
