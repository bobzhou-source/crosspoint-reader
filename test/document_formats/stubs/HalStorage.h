#pragma once
#include "Print.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <utility>

class HalFile final : public Print {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) noexcept { *this = std::move(other); }
  HalFile& operator=(HalFile&& other) noexcept {
    if (this != &other) {
      close();
      handle = std::exchange(other.handle, nullptr);
      path = std::move(other.path);
      directory = std::exchange(other.directory, false);
      iterator = std::move(other.iterator);
    }
    return *this;
  }
  bool open(const std::string& name, const char* mode) {
    close(); path = name;
    directory = std::filesystem::is_directory(path);
    if (directory) { iterator = std::filesystem::directory_iterator(path); return true; }
    handle = std::fopen(name.c_str(), mode);
    return handle;
  }
  bool close() {
    bool ok = true;
    if (handle) { ok = std::fclose(handle) == 0; handle = nullptr; }
    directory = false;
    iterator = {};
    return ok;
  }
  operator bool() const { return handle || directory; }
  bool isDirectory() const { return directory; }
  bool isOpen() const { return bool(*this); }
  uint64_t fileSize64() const { struct stat st {}; return ::stat(path.c_str(), &st) ? 0 : st.st_size; }
  size_t size() const { return fileSize64(); }
  size_t fileSize() const { return size(); }
  uint32_t modificationTime() const { struct stat st {}; return ::stat(path.c_str(), &st) ? 0 : st.st_mtime; }
  size_t position() const { return handle ? std::ftell(handle) : 0; }
  bool seek(size_t offset) { return handle && std::fseek(handle, offset, SEEK_SET) == 0; }
  bool seekSet(size_t offset) { return seek(offset); }
  bool seekCur(int64_t offset) { return handle && std::fseek(handle, offset, SEEK_CUR) == 0; }
  int available() const { return handle ? static_cast<int>(size() - position()) : 0; }
  int read(void* output, size_t count) { return handle ? static_cast<int>(std::fread(output, 1, count, handle)) : -1; }
  int read() { return handle ? std::fgetc(handle) : -1; }
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t count) override {
    if (const char* limit = std::getenv("FORMAT_FAIL_WRITE_AFTER")) {
      const uint64_t ceiling = std::strtoull(limit, nullptr, 10);
      if (written >= ceiling) return 0;
      count = std::min<uint64_t>(count, ceiling - written);
    }
    const size_t result = handle ? std::fwrite(data, 1, count, handle) : 0;
    written += result;
    return result;
  }
  size_t write(const void* data, size_t count) { return write(static_cast<const uint8_t*>(data), count); }
  void flush() override { if (handle) std::fflush(handle); }
  size_t getName(char* output, size_t capacity) {
    const std::string name = path.filename().string();
    std::snprintf(output, capacity, "%s", name.c_str());
    return std::min(name.size(), capacity - 1);
  }
  HalFile openNextFile() {
    HalFile result;
    if (directory && iterator != std::filesystem::directory_iterator{}) {
      const auto next = iterator->path();
      ++iterator;
      result.open(next.string(), "rb");
    }
    return result;
  }

 private:
  FILE* handle = nullptr;
  std::filesystem::path path;
  bool directory = false;
  std::filesystem::directory_iterator iterator;
  inline static uint64_t written = 0;
};

class HalStorage {
  static std::string mapped(const std::string& path) {
    if (const char* root = std::getenv("FORMAT_STORAGE_ROOT"); root && !path.empty() && path.front() == '/') {
      return std::string(root) + path;
    }
    return path;
  }

 public:
  static HalStorage& getInstance() { static HalStorage instance; return instance; }
  bool ready() { return true; }
  bool openFileForRead(const char*, const std::string& path, HalFile& output) { return output.open(mapped(path), "rb"); }
  bool openFileForRead(const char* module, const char* path, HalFile& output) { return openFileForRead(module, std::string(path), output); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& output) { return output.open(mapped(path), "wb+"); }
  bool openFileForWrite(const char* module, const char* path, HalFile& output) { return openFileForWrite(module, std::string(path), output); }
  HalFile open(const char* path) { HalFile file; file.open(mapped(path), "rb"); return file; }
  bool exists(const char* path) { return std::filesystem::exists(mapped(path)); }
  bool ensureDirectoryExists(const char* path) {
    const std::string destination = mapped(path);
    std::error_code ec;
    return std::filesystem::is_directory(destination) || std::filesystem::create_directories(destination, ec);
  }
  bool mkdir(const char* path, bool = true) { return ensureDirectoryExists(path); }
  bool remove(const char* path) { std::error_code ec; return std::filesystem::remove(mapped(path), ec); }
  bool remove(const std::string& path) { return remove(path.c_str()); }
  bool removeDir(const char* path) { std::error_code ec; std::filesystem::remove_all(mapped(path), ec); return !ec; }
  bool rename(const char* from, const char* to) {
    std::error_code ec; std::filesystem::rename(mapped(from), mapped(to), ec); return !ec;
  }
};
#define Storage HalStorage::getInstance()
