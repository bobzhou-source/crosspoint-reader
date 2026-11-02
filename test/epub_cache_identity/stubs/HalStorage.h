#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

// Real metadata headers own HalFiles, but these constructor/cache tests never
// open them. Fail loudly if a new path unexpectedly performs file I/O here.
class HalFile {
 public:
  size_t position() const { return 0; }
  int read(void*, size_t) { throw std::logic_error("Unexpected EPUB metadata read"); }
  size_t write(const void*, size_t) { throw std::logic_error("Unexpected EPUB metadata write"); }
  bool seek(size_t) { throw std::logic_error("Unexpected EPUB metadata seek"); }
};

// Map SD-card absolute paths into a fresh temporary directory. Never operate on
// host absolute paths or permit parent traversal outside the test sandbox.
class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  std::filesystem::path root;
  std::vector<std::string> removalAttempts;
  bool failRemoval = false;

  std::filesystem::path resolve(const char* name) const {
    const std::filesystem::path path(name);
    if (root.empty() || !path.is_absolute()) throw std::logic_error("Unbound cache test sandbox");
    for (const auto& component : path) {
      if (component == "..") throw std::logic_error("Cache test path escapes sandbox");
    }
    return root / path.relative_path();
  }

  bool exists(const char* name) const { return std::filesystem::exists(resolve(name)); }

  bool mkdir(const char* name) { return std::filesystem::create_directories(resolve(name)); }

  bool removeDir(const char* name) {
    removalAttempts.emplace_back(name);
    const auto path = resolve(name);
    if (failRemoval) return false;
    std::error_code error;
    const auto count = std::filesystem::remove_all(path, error);
    return count != 0 && !error;
  }
};

#define Storage HalStorage::getInstance()
