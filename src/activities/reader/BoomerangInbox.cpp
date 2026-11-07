#include "BoomerangInbox.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include "Boomerang.h"
#include "CrossPointSettings.h"

namespace {

constexpr size_t NAME_BUFFER_SIZE = 512;

// Progress lives in the book's cache directory; deriving it needs only the
// constructor's path hash, not a parsed EPUB.
bool hasSavedProgress(const std::string& path) {
  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) {
    // Cannot tell unread from finished: keep the item out of the rotation rather
    // than re-surface something already read.
    return true;
  }
  return Storage.exists((epub->getCachePath() + "/progress.bin").c_str());
}

std::string inboxPathFor(const char* name) {
  std::string path(boomerang::kInboxFolder);
  path += '/';
  path += name;
  return path;
}

}  // namespace

std::string findInboxBreakBook() {
  auto dir = Storage.open(boomerang::kInboxFolder);
  if (!dir || !dir.isDirectory()) {
    return {};
  }
  dir.rewindDirectory();

  const auto nameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!nameBuffer) {
    LOG_ERR("BRM", "OOM: %d bytes", static_cast<int>(NAME_BUFFER_SIZE));
    dir.close();
    return {};
  }

  std::string best;
  while (auto entry = dir.openNextFile()) {
    if (entry.isDirectory()) {
      continue;
    }
    const size_t nameLength = entry.getName(nameBuffer.get(), NAME_BUFFER_SIZE);
    if (nameLength == 0 || nameBuffer[0] == '\0') {
      continue;
    }
    nameBuffer[NAME_BUFFER_SIZE - 1] = '\0';
    if (!SETTINGS.showHiddenFiles && nameBuffer[0] == '.') {
      continue;
    }
    if (!FsHelpers::hasEpubExtension(nameBuffer.get())) {
      continue;
    }
    std::string path = inboxPathFor(nameBuffer.get());
    if (!boomerang::prefersBreakCandidate(best, path, hasSavedProgress(path))) {
      continue;
    }
    best.swap(path);
  }
  dir.close();

  if (best.empty()) {
    LOG_DBG("BRM", "No unread EPUB in %s", boomerang::kInboxFolder);
  } else {
    LOG_DBG("BRM", "Quick break item: %s", best.c_str());
  }
  return best;
}
