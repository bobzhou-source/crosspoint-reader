#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

inline std::map<std::string, std::vector<uint8_t>> testFiles;
inline unsigned testWriteCalls = 0;
inline unsigned testSyncs = 0;
inline unsigned testRenames = 0;
inline bool testFailSync = false;
class HalFile {
 public:
  std::vector<uint8_t>* bytes = nullptr;
  size_t offset = 0;
  size_t size() const { return bytes ? bytes->size() : 0; }
  int read(void* data, size_t size) {
    if (!bytes) return -1;
    const auto n = std::min(size, bytes->size() - offset);
    memcpy(data, bytes->data() + offset, n);
    offset += n;
    return static_cast<int>(n);
  }
  size_t write(const void* data, size_t size) {
    if (!bytes) return 0;
    ++testWriteCalls;
    const auto* p = static_cast<const uint8_t*>(data);
    bytes->insert(bytes->end(), p, p + size);
    return size;
  }
  bool sync() {
    ++testSyncs;
    return !testFailSync;
  }
  bool close() {
    bytes = nullptr;
    return true;
  }
};
class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    auto it = testFiles.find(path);
    if (it == testFiles.end()) return false;
    file.bytes = &it->second;
    file.offset = 0;
    return true;
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    file.bytes = &testFiles[path];
    file.bytes->clear();
    file.offset = 0;
    return true;
  }
  bool ensureDirectoryExists(const char*) { return true; }
  bool exists(const char* path) { return testFiles.count(path); }
  bool remove(const char* path) { return testFiles.erase(path) != 0; }
  bool rename(const char* from, const char* to) {
    if (!testFiles.count(from) || testFiles.count(to)) return false;
    testFiles[to] = std::move(testFiles[from]);
    testFiles.erase(from);
    ++testRenames;
    return true;
  }
};
#define Storage HalStorage::getInstance()
