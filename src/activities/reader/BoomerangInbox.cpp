#include "BoomerangInbox.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "Boomerang.h"
#include "CrossPointSettings.h"
#include "util/ConceptEngine.h"

namespace {

constexpr size_t NAME_BUFFER_SIZE = 512;
// Inbox EPUBs ranked against the shelf; any past this take the lowest-path rule only.
constexpr size_t MAX_RANKED = 64;

enum class InboxProbe : uint8_t { Unknown, Empty, Present };
InboxProbe inboxProbe = InboxProbe::Unknown;

void assignInboxPath(std::string& path, const std::string_view name) {
  path.assign(boomerang::kInboxFolder);
  path += '/';
  path.append(name.data(), name.size());
}

// Progress lives in the book's cache directory, named the way
// Epub(path, "/.crosspoint") names it, so no EPUB has to be constructed.
bool hasSavedProgress(const std::string& path) {
  const std::string progress =
      "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(path)) + "/progress.bin";
  return Storage.exists(progress.c_str());
}

// Calls fn(name) for each visible EPUB directly in the Inbox until it returns false.
template <typename Fn>
void forEachInboxEpub(Fn&& fn) {
  auto dir = Storage.open(boomerang::kInboxFolder);
  if (!dir || !dir.isDirectory()) {
    return;
  }
  dir.rewindDirectory();

  const auto nameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!nameBuffer) {
    LOG_ERR("BRM", "OOM: %d bytes", static_cast<int>(NAME_BUFFER_SIZE));
    dir.close();
    return;
  }

  while (auto entry = dir.openNextFile()) {
    if (entry.isDirectory()) {
      continue;
    }
    const size_t nameLength = entry.getName(nameBuffer.get(), NAME_BUFFER_SIZE);
    entry.close();
    if (nameLength == 0 || nameBuffer[0] == '\0') {
      continue;
    }
    nameBuffer[NAME_BUFFER_SIZE - 1] = '\0';
    if (!SETTINGS.showHiddenFiles && nameBuffer[0] == '.') {
      continue;
    }
    const std::string_view name{nameBuffer.get()};
    if (!FsHelpers::hasEpubExtension(name)) {
      continue;
    }
    if (!fn(name)) {
      break;
    }
  }
  dir.close();
}

// The unread item whose shelf vector best matches the reading context; empty
// without a shelf, a context, scored items, or an unread one among them.
std::string closestUnread(const std::vector<std::string>& names) {
  if (names.empty() || !Storage.exists(ConceptEngine::SHELF_PATH)) {
    return {};
  }
  int8_t context[ConceptEngine::DIM];
  if (!ConceptEngine::readingContext(context)) {
    return {};
  }
  const size_t count = names.size();
  uint64_t hashes[MAX_RANKED];
  int32_t dots[MAX_RANKED];
  std::string path;
  for (size_t i = 0; i < count; i++) {
    assignInboxPath(path, names[i]);
    hashes[i] = ConceptEngine::fnv1a64(path);
  }
  if (ConceptEngine::scoreShelf(context, hashes, count, dots) == 0) {
    return {};
  }
  uint8_t order[MAX_RANKED];
  size_t scored = 0;
  for (size_t i = 0; i < count; i++) {
    if (dots[i] != INT32_MIN) {  // INT32_MIN: the shelf has no record for it
      order[scored++] = static_cast<uint8_t>(i);
    }
  }
  std::sort(order, order + scored, [&](const uint8_t a, const uint8_t b) {
    if (dots[a] != dots[b]) {
      return dots[a] > dots[b];
    }
    return boomerang::compareIgnoreCase(names[a], names[b]) < 0;
  });
  for (size_t k = 0; k < scored; k++) {
    assignInboxPath(path, names[order[k]]);
    if (!hasSavedProgress(path)) {
      return path;
    }
  }
  return {};
}

// The unread item with the lowest path among `names` and `overflowBest`.
std::string lowestUnread(std::vector<std::string>& names, std::string overflowBest) {
  std::sort(names.begin(), names.end(),
            [](const std::string& a, const std::string& b) { return boomerang::compareIgnoreCase(a, b) < 0; });
  std::string path;
  for (const auto& name : names) {
    assignInboxPath(path, name);
    // Sorted: once a name cannot beat the overflow pick, no later one can.
    if (!boomerang::prefersBreakCandidate(overflowBest, path, false)) {
      break;
    }
    if (!hasSavedProgress(path)) {
      return path;
    }
  }
  return overflowBest;
}

}  // namespace

std::string findInboxBreakBook() {
  std::vector<std::string> names;
  std::string overflowBest;
  std::string path;
  forEachInboxEpub([&](const std::string_view name) {
    if (names.size() < MAX_RANKED) {
      names.emplace_back(name);
      return true;
    }
    assignInboxPath(path, name);
    if (boomerang::prefersBreakCandidate(overflowBest, path, false) && !hasSavedProgress(path)) {
      overflowBest = path;
    }
    return true;
  });

  std::string best = closestUnread(names);
  if (best.empty()) {
    best = lowestUnread(names, std::move(overflowBest));
  } else {
    LOG_DBG("BRM", "Quick break by reading context");
  }

  if (best.empty()) {
    LOG_DBG("BRM", "No unread EPUB in %s", boomerang::kInboxFolder);
  } else {
    LOG_DBG("BRM", "Quick break item: %s", best.c_str());
  }
  return best;
}

bool hasInboxBreakBook() {
  if (inboxProbe == InboxProbe::Unknown) {
    bool found = Storage.exists(ConceptEngine::SHELF_PATH);
    if (!found) {
      forEachInboxEpub([&found](std::string_view) {
        found = true;
        return false;
      });
    }
    inboxProbe = found ? InboxProbe::Present : InboxProbe::Empty;
  }
  return inboxProbe == InboxProbe::Present;
}

void forgetInboxBreakBook() { inboxProbe = InboxProbe::Empty; }
