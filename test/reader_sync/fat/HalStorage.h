#pragma once
// Minimal HAL for the pinned-SdFat privacy regression. Unlike the POSIX
// adapter, this one is the actual SdFat FatLib: alias opens, LFN ordinals and
// dirIndex all come from the production dependency.
#include <FatLib/FatLib.h>

#include <cstddef>
#include <cstdint>
#include <utility>

// Mounted by privacy_host.cpp before ReaderSyncStorage is exercised.
inline FatVolume* readerSyncFat = nullptr;

class HalFile {
  FatFile file;

 public:
  HalFile() = default;
  explicit HalFile(FatFile&& opened) : file(static_cast<FatFile&&>(opened)) {}
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  explicit operator bool() const { return file.isOpen(); }
  bool isDirectory() const { return file.isDir(); }
  size_t getName(char* name, size_t len) { return file.getName(name, len); }
  uint32_t dirIndex() const { return file.dirIndex(); }
  size_t size() const { return file.fileSize(); }
  void rewindDirectory() { file.rewind(); }
  HalFile openNextFile() {
    FatFile next;
    if (!next.openNext(&file)) return {};
    return HalFile(static_cast<FatFile&&>(next));
  }
  int read() { return file.read(); }
  int read(void* buffer, size_t count) { return file.read(buffer, count); }
  size_t write(const uint8_t* buffer, size_t count) { return file.write(buffer, count); }
  bool sync() { return file.sync(); }
  bool close() { return file.close(); }
};

class HalStorage {
 public:
  HalFile open(const char* path, const oflag_t oflag = O_RDONLY) {
    FatFile opened;
    if (!readerSyncFat || !opened.open(readerSyncFat, path, oflag)) return {};
    return HalFile(static_cast<FatFile&&>(opened));
  }
  bool exists(const char* path) { return readerSyncFat && readerSyncFat->exists(path); }
  bool remove(const char* path) { return readerSyncFat && readerSyncFat->remove(path); }
  bool rename(const char* oldPath, const char* newPath) {
    return readerSyncFat && readerSyncFat->rename(oldPath, newPath);
  }
  bool mkdir(const char* path, bool pFlag = true) { return readerSyncFat && readerSyncFat->mkdir(path, pFlag); }
};

inline HalStorage Storage;
