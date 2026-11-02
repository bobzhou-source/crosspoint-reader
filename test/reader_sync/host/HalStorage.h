#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// The production protocol owns all validation/atomic-install decisions. This
// adapter maps only HAL filesystem calls to a disposable POSIX directory.
//
// It models the two FAT behaviors the privacy resolver depends on: a handle
// keeps the name it was opened with (an 8.3 alias reports the alias, exactly
// like SdFat's m_lfnOrd==0 handle), and every handle exposes a stable
// directory-entry identity so an alias can be matched against its parent's
// enumeration. Real 8.3 name generation is covered by the pinned-SdFat image
// regression (test/reader_sync/run_fat_privacy.py), not by this adapter.
namespace reader_test {
inline uint32_t entryIdentity(const struct stat& info) {
  uint64_t value = (static_cast<uint64_t>(info.st_dev) << 32) ^ static_cast<uint64_t>(info.st_ino);
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdULL;
  value ^= value >> 33;
  return static_cast<uint32_t>(value) | 1u;  // 0 means unknown identity
}
}  // namespace reader_test

class HalFile {
  FILE* file = nullptr;
  std::filesystem::path path;
  std::string entryName;
  uint32_t identity = 0;
  std::unique_ptr<std::filesystem::directory_iterator> directory;
 public:
  HalFile() = default;
  explicit HalFile(std::filesystem::path path, int flags = O_RDONLY) : path(std::move(path)) {
    entryName = this->path.filename().string();
    struct stat info {};
    // Follow links: an alternate spelling resolves to the same entry, which is
    // what SdFat's LFN/SFN lookup does for a FAT directory entry.
    if (::stat(this->path.c_str(), &info) == 0) identity = reader_test::entryIdentity(info);
    std::error_code error;
    if (std::filesystem::exists(this->path, error)) this->path = std::filesystem::canonical(this->path, error);
    if (std::filesystem::is_directory(this->path, error)) directory = std::make_unique<std::filesystem::directory_iterator>(this->path);
    else file = fopen(this->path.c_str(), (flags & O_WRONLY) ? ((flags & O_TRUNC) ? "wb" : "ab") : "rb");
  }
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) noexcept : file(other.file), path(std::move(other.path)), entryName(std::move(other.entryName)),
                                      identity(other.identity), directory(std::move(other.directory)) { other.file = nullptr; }
  HalFile& operator=(HalFile&& other) noexcept {
    if (&other != this) {
      close();
      file = other.file;
      other.file = nullptr;
      path = std::move(other.path);
      entryName = std::move(other.entryName);
      identity = other.identity;
      directory = std::move(other.directory);
    }
    return *this;
  }
  explicit operator bool() const { return file || directory; }
  bool isDirectory() const { return bool(directory); }
  size_t size() const { std::error_code e; auto n = std::filesystem::file_size(path, e); return e ? 0 : n; }
  int read(void* buffer, size_t n) { return file ? static_cast<int>(fread(buffer, 1, n, file)) : -1; }
  int read() { return file ? fgetc(file) : -1; }
  size_t write(const uint8_t* bytes, size_t n) {
    if (getenv("READER_TEST_WRITE_FAIL") && path.extension() == ".part") return 0;
    return file ? fwrite(bytes, 1, n, file) : 0;
  }
  bool sync() { return file && fflush(file) == 0 && fsync(fileno(file)) == 0; }
  bool close() { const bool ok = !file || fclose(file) == 0; file = nullptr; directory.reset(); return ok; }
  size_t getName(char* out, size_t n) {
    if (!n) return 0;
    const size_t length = std::min(entryName.size(), n - 1);
    memcpy(out, entryName.data(), length);
    out[length] = '\0';
    return length;
  }
  uint32_t dirIndex() const { return identity; }
  void rewindDirectory() {
    if (directory) directory = std::make_unique<std::filesystem::directory_iterator>(path);
  }
  HalFile openNextFile() {
    if (!directory || *directory == std::filesystem::directory_iterator{}) return {};
    auto next = (**directory).path();
    ++(*directory);
    return HalFile(next);
  }
};
class HalStorage {
 public:
  std::filesystem::path root;
  std::filesystem::path resolve(const char* path) const { return root / std::filesystem::path(path).relative_path(); }
  HalFile open(const char* path, int flags = O_RDONLY) { return HalFile(resolve(path), flags); }
  bool exists(const char* path) { return std::filesystem::exists(resolve(path)); }
  bool remove(const char* path) { std::error_code e; return std::filesystem::remove(resolve(path), e); }
  bool mkdir(const char* path) { return ensureDirectoryExists(path); }
  bool ensureDirectoryExists(const char* path) {
    std::error_code e; std::filesystem::create_directories(resolve(path), e); return !e;
  }
  bool rename(const char* before, const char* after) {
    if (exists(after)) return false;
    if (getenv("READER_TEST_RENAME_FAIL") && std::string(after).find("/books/Inbox/") == 0) return false;
    std::error_code e; std::filesystem::rename(resolve(before), resolve(after), e);
    if (!e) {
      const int fd = ::open(resolve(after).parent_path().c_str(), O_RDONLY);
      if (fd >= 0) { fsync(fd); ::close(fd); }
    }
    return !e;
  }
  uint64_t freeBytes() {
    if (const char* bytes = getenv("READER_TEST_FREE_BYTES")) return strtoull(bytes, nullptr, 10);
    return std::filesystem::space(root).available;
  }
};
inline HalStorage Storage;
