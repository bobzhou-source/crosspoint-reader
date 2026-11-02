// Privacy regression over the pinned SdFat implementation.
//
// The fixture formats an in-memory FAT16 image, creates the real
// /.crosspoint tree and public control files, reads the short names SdFat
// actually generated from the directory entries, and then:
//   1. reproduces the old getName()-on-alias-open guard and shows the alias
//      exposes the device key through SdFat itself;
//   2. asserts the production privateWebPath() rejects root and nested aliases;
//   3. asserts ordinary '~'/'%'/long public names and their real SFN aliases
//      remain readable.
// Nothing here is mocked: block I/O is a RAM sector device, and all name and
// directory-entry behavior comes from the pinned FatLib sources.
#include <FatLib/FatLib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ReaderSyncProtocol.h"

namespace {

constexpr uint32_t kSectorSize = 512;
constexpr uint32_t kSectors = 32768;  // 16 MiB, FAT16 as the formatter picks
constexpr char kDeviceKey[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

class MemoryDevice final : public FsBlockDeviceInterface {
  std::vector<uint8_t> image;

 public:
  explicit MemoryDevice(size_t sectors) : image(sectors * kSectorSize, 0) {}

  bool isBusy() override { return false; }
  bool readSector(Sector_t sector, uint8_t* dst) override { return readSectors(sector, dst, 1); }
  bool readSectors(Sector_t sector, uint8_t* dst, size_t count) override {
    if (sector + count > sectorCount()) return false;
    memcpy(dst, image.data() + static_cast<size_t>(sector) * kSectorSize, count * kSectorSize);
    return true;
  }
  Sector_t sectorCount() override { return static_cast<Sector_t>(image.size() / kSectorSize); }
  bool syncDevice() override { return true; }
  bool writeSector(Sector_t sector, const uint8_t* src) override { return writeSectors(sector, src, 1); }
  bool writeSectors(Sector_t sector, const uint8_t* src, size_t count) override {
    if (sector + count > sectorCount()) return false;
    memcpy(image.data() + static_cast<size_t>(sector) * kSectorSize, src, count * kSectorSize);
    return true;
  }
};

bool writeFile(FatVolume& volume, const char* path, const char* text) {
  FatFile file;
  if (!file.open(&volume, path, O_WRONLY | O_CREAT | O_TRUNC)) return false;
  const size_t length = strlen(text);
  const bool written = file.write(text, length) == length && file.sync();
  return file.close() && written;
}

std::string readFile(FatVolume& volume, const char* path) {
  FatFile file;
  if (!file.open(&volume, path, O_RDONLY)) return {};
  std::string out;
  char buffer[64];
  int count;
  while ((count = file.read(buffer, sizeof(buffer))) > 0) out.append(buffer, static_cast<size_t>(count));
  file.close();
  return out;
}

struct Entry {
  std::string longName;
  std::string shortName;
  bool isDirectory = false;
};

bool equalNoCase(const std::string& a, const char* b) {
  if (a.size() != strlen(b)) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    unsigned char x = static_cast<unsigned char>(a[i]);
    unsigned char y = static_cast<unsigned char>(b[i]);
    if (x >= 'A' && x <= 'Z') x = static_cast<unsigned char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<unsigned char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

// Directory-enumerated entries restore the LFN ordinal, so this is the only
// source of a canonical long name.
bool findEntry(FatVolume& volume, const char* directoryPath, const char* longName, Entry& found) {
  FatFile directory;
  if (!directory.open(&volume, directoryPath, O_RDONLY) || !directory.isDir()) return false;
  for (;;) {
    FatFile entry;
    if (!entry.openNext(&directory)) break;
    char lfn[256];
    char sfn[13];
    const size_t lfnLength = entry.getName(lfn, sizeof(lfn));
    const size_t sfnLength = entry.getSFN(sfn, sizeof(sfn));
    if (!lfnLength || !sfnLength) {
      entry.close();
      directory.close();
      return false;
    }
    if (equalNoCase(std::string(lfn, lfnLength), longName)) {
      found.longName.assign(lfn, lfnLength);
      found.shortName.assign(sfn, sfnLength);
      found.isDirectory = entry.isDir();
      entry.close();
      directory.close();
      return true;
    }
    entry.close();
  }
  directory.close();
  return false;
}

// The reviewed implementation: trust getName() on the alias-opened handle.
bool oldPrivateWebPath(FatVolume& volume, const char* path) {
  std::string resolved;
  const char* p = path;
  while (*p) {
    while (*p == '/' || *p == '\\') ++p;
    const char* start = p;
    while (*p && *p != '/' && *p != '\\') ++p;
    const size_t n = static_cast<size_t>(p - start);
    if (!n) continue;
    if (start[0] == '.' || start[n - 1] == ' ' || start[n - 1] == '.') return true;
    resolved += '/';
    resolved.append(start, n);
    FatFile component;
    if (component.open(&volume, resolved.c_str(), O_RDONLY)) {
      char longName[256];
      const size_t length = component.getName(longName, sizeof(longName));
      if (!length || longName[0] == '.') {
        component.close();
        return true;
      }
      const size_t slash = resolved.rfind('/');
      resolved.resize(slash + 1);
      resolved.append(longName, length);
      component.close();
    }
  }
  return false;
}

}  // namespace

int main() {
  MemoryDevice device(kSectors);
  FatFormatter formatter;
  uint8_t sectorBuffer[kSectorSize];
  if (!formatter.format(&device, sectorBuffer)) {
    std::fprintf(stderr, "FAIL: FAT format\n");
    return 1;
  }
  FatVolume volume;
  if (!volume.begin(&device, true, 1)) {
    std::fprintf(stderr, "FAIL: FAT mount\n");
    return 2;
  }
  readerSyncFat = &volume;

  const bool fixture =
      volume.mkdir("/.crosspoint", true) && volume.mkdir("/.crosspoint/reader-sync", true) &&
      writeFile(volume, "/.crosspoint/reader-sync.json", kDeviceKey) &&
      writeFile(volume, "/.crosspoint/reader-sync/status.json", "status") &&
      volume.mkdir("/books/Inbox", true) && writeFile(volume, "/books/Inbox/notes~draft.json", "notes") &&
      writeFile(volume, "/books/Inbox/50% complete.epub", "percent") &&
      writeFile(volume, "/books/Inbox/a-very-long-public-name.epub", "long") &&
      writeFile(volume, "/readme.txt", "readme") && writeFile(volume, "/draft~1.txt", "tilde") &&
      writeFile(volume, "/50%.txt", "short-percent") &&
      writeFile(volume, "/.crosspoint-public.txt", "public-dot");
  if (!fixture) {
    std::fprintf(stderr, "FAIL: fixture creation\n");
    return 3;
  }

  Entry privateRoot;
  Entry nestedDir;
  Entry configFile;
  Entry notes;
  Entry percent;
  Entry longName;
  if (!findEntry(volume, "/", ".crosspoint", privateRoot) ||
      !findEntry(volume, "/.crosspoint", "reader-sync", nestedDir) ||
      !findEntry(volume, "/.crosspoint", "reader-sync.json", configFile) ||
      !findEntry(volume, "/books/Inbox", "notes~draft.json", notes) ||
      !findEntry(volume, "/books/Inbox", "50% complete.epub", percent) ||
      !findEntry(volume, "/books/Inbox", "a-very-long-public-name.epub", longName)) {
    std::fprintf(stderr, "FAIL: directory-entry lookup\n");
    return 4;
  }
  if (privateRoot.shortName == ".crosspoint" || notes.shortName == "notes~draft.json") {
    std::fprintf(stderr, "FAIL: expected generated 8.3 names, got %s / %s\n", privateRoot.shortName.c_str(),
                 notes.shortName.c_str());
    return 5;
  }

  // Pinned SdFat behavior: opening through the alias leaves m_lfnOrd zero, so
  // getName() returns the short name rather than the private long name.
  FatFile alias;
  if (!alias.open(&volume, ("/" + privateRoot.shortName).c_str(), O_RDONLY) || !alias.isDir()) {
    std::fprintf(stderr, "FAIL: private root alias did not open\n");
    return 6;
  }
  char aliasName[256] = {};
  if (!alias.getName(aliasName, sizeof(aliasName)) || alias.isLFN() ||
      !equalNoCase(aliasName, privateRoot.shortName.c_str())) {
    std::fprintf(stderr, "FAIL: alias-open exposed '%s' (isLFN=%d)\n", aliasName, alias.isLFN());
    return 7;
  }
  alias.close();

  FatFile configAlias;
  if (!configAlias.open(&volume, ("/.crosspoint/" + configFile.shortName).c_str(), O_RDONLY) ||
      configAlias.isLFN()) {
    std::fprintf(stderr, "FAIL: config alias did not reproduce alias-open behavior\n");
    return 8;
  }
  configAlias.close();

  const std::string rootAlias = "/" + privateRoot.shortName;
  const std::string nestedAlias = rootAlias + "/" + nestedDir.shortName;
  const std::string directConfigAlias = "/.crosspoint/" + configFile.shortName;

  // Reproduction of the reviewed failure through real SdFat file access.
  if (readFile(volume, (rootAlias + "/reader-sync.json").c_str()) != kDeviceKey) {
    std::fprintf(stderr, "FAIL: alias path did not expose device_key\n");
    return 9;
  }
  if (readFile(volume, directConfigAlias.c_str()) != kDeviceKey) {
    std::fprintf(stderr, "FAIL: config SFN alias did not expose device_key\n");
    return 10;
  }
  if (readFile(volume, (nestedAlias + "/status.json").c_str()) != "status") {
    std::fprintf(stderr, "FAIL: nested alias path did not expose private state\n");
    return 11;
  }
  if (oldPrivateWebPath(volume, (rootAlias + "/reader-sync.json").c_str()) ||
      oldPrivateWebPath(volume, (nestedAlias + "/status.json").c_str())) {
    std::fprintf(stderr, "FAIL: old guard unexpectedly rejected a real alias; fixture assumption broken\n");
    return 12;
  }

  // The fix: every alias spelling of the private subtree is rejected.
  const std::vector<std::string> denied = {
      "/.crosspoint/reader-sync.json",
      "/.CROSSPOINT/reader-sync.json",
      rootAlias,
      rootAlias + "/reader-sync.json",
      rootAlias + "/" + configFile.shortName,
      nestedAlias + "/status.json",
      directConfigAlias,
      rootAlias + "/",
  };
  for (const std::string& path : denied) {
    if (!reader_sync::privateWebPath(path.c_str())) {
      std::fprintf(stderr, "FAIL: private path allowed: %s\n", path.c_str());
      return 13;
    }
  }
  // SdFat matches short names case-insensitively and normalizes dot segments
  // before opening, so these spellings reach the same private entry.
  std::string lowerAlias = rootAlias;
  for (char& c : lowerAlias) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  if (!reader_sync::privateWebPath(lowerAlias.c_str()) ||
      !reader_sync::privateWebPath(("/x/../" + privateRoot.shortName).c_str())) {
    std::fprintf(stderr, "FAIL: normalized alias spelling was allowed\n");
    return 14;
  }

  // Positive controls: ordinary public names, including real alternate names,
  // remain accessible and are not classified as private.
  const std::vector<std::string> publicAliases = {
      "/books/Inbox/" + notes.shortName,
      "/books/Inbox/" + percent.shortName,
      "/books/Inbox/" + longName.shortName,
  };
  std::vector<std::string> allowed = {
      "/readme.txt",
      "/draft~1.txt",
      "/50%.txt",
      "/.crosspoint-public.txt",
      "/books/Inbox/notes~draft.json",
      "/books/Inbox/50% complete.epub",
      "/books/Inbox/a-very-long-public-name.epub",
  };
  allowed.insert(allowed.end(), publicAliases.begin(), publicAliases.end());
  for (const std::string& path : allowed) {
    if (reader_sync::privateWebPath(path.c_str())) {
      std::fprintf(stderr, "FAIL: public path denied: %s\n", path.c_str());
      return 15;
    }
  }
  if (readFile(volume, publicAliases[0].c_str()) != "notes" ||
      readFile(volume, publicAliases[1].c_str()) != "percent" ||
      readFile(volume, publicAliases[2].c_str()) != "long") {
    std::fprintf(stderr, "FAIL: public alias access broke\n");
    return 16;
  }

  std::printf("PASS: pinned SdFat alias-open returns '%s'; private root/nested aliases denied; public controls readable\n",
              privateRoot.shortName.c_str());
  return 0;
}
