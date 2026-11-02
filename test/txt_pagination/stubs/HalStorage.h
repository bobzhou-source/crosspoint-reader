#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

// Host stand-in for the SD storage HAL: one in-memory source file with exact IO counters and
// scripted faults. Read-call numbers in the fault script are 1-based and count from the last reset.
struct FakeSource {
  std::string path = "/book.txt";
  std::vector<uint8_t> data;

  // Counters
  int openCount = 0;
  int openWhileOpen = 0;  // an open into a HalFile that was still open
  int seekCount = 0;
  int readCalls = 0;
  int shortReads = 0;  // calls that returned fewer bytes than requested
  int errorReads = 0;  // calls that returned a negative count
  size_t requestedBytes = 0;
  size_t actualBytes = 0;

  // Fault script
  bool failOpen = false;
  bool failSeek = false;
  size_t maxBytesPerRead = std::numeric_limits<size_t>::max();  // short progress per call
  size_t truncateAt = std::numeric_limits<size_t>::max();       // bytes at/after this offset read as EOF
  int errorOnReadCall = 0;                                      // returns -1
  int zeroOnReadCall = 0;                                       // returns 0
  int oversizeOnReadCall = 0;                                   // returns count + 1
};

class HalStorage;

class HalFile {
 public:
  HalFile() = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  ~HalFile() { close(); }

  bool isOpen() const { return source_ != nullptr; }

  bool close() {
    if (source_ == nullptr) return false;
    source_ = nullptr;
    return true;
  }

  bool seek(const size_t pos) {
    if (source_ == nullptr || source_->failSeek) return false;
    source_->seekCount++;
    position_ = pos;
    return true;
  }

  int read(void* dst, const size_t count) {
    if (source_ == nullptr) return -1;
    FakeSource& s = *source_;
    s.readCalls++;
    s.requestedBytes += count;
    if (s.errorOnReadCall == s.readCalls) {
      s.errorReads++;
      return -1;
    }
    if (s.zeroOnReadCall == s.readCalls) {
      s.shortReads++;
      return 0;
    }
    const size_t limit = std::min(s.data.size(), s.truncateAt);
    size_t n = position_ < limit ? std::min(count, limit - position_) : 0;
    n = std::min(n, s.maxBytesPerRead);
    if (n > 0) std::memcpy(dst, s.data.data() + position_, n);
    position_ += n;
    s.actualBytes += n;
    if (n < count) s.shortReads++;
    if (s.oversizeOnReadCall == s.readCalls) return static_cast<int>(count + 1);
    return static_cast<int>(n);
  }

 private:
  friend class HalStorage;
  FakeSource* source_ = nullptr;
  size_t position_ = 0;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  FakeSource& source() { return source_; }

  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    if (file.isOpen()) source_.openWhileOpen++;
    if (source_.failOpen || path != source_.path) return false;
    source_.openCount++;
    file.source_ = &source_;
    file.position_ = 0;
    return true;
  }

 private:
  FakeSource source_;
};

#define Storage HalStorage::getInstance()
