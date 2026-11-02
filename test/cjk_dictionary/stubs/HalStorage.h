#pragma once
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
constexpr int O_WRITE = 1, O_CREAT = 2, O_TRUNC = 4;
class HalFile {
  FILE* file = nullptr;

 public:
  HalFile() = default;
  HalFile(HalFile&& other) : file(std::exchange(other.file, nullptr)) {}
  HalFile& operator=(HalFile&& other) {
    close();
    file = std::exchange(other.file, nullptr);
    return *this;
  }
  ~HalFile() { close(); }
  explicit operator bool() const { return file; }
  bool open(const std::string& path, const char* mode) {
    close();
    file = fopen(path.c_str(), mode);
    return file;
  }
  void close() {
    if (file) fclose(file);
    file = nullptr;
  }
  int read() { return file ? fgetc(file) : -1; }
  int read(void* out, size_t n) { return file ? static_cast<int>(fread(out, 1, n, file)) : -1; }
  int write(const void* data, size_t n) { return file ? static_cast<int>(fwrite(data, 1, n, file)) : -1; }
  bool seekSet(size_t pos) { return file && fseek(file, pos, SEEK_SET) == 0; }
  size_t position() { return ftell(file); }
  size_t fileSize() {
    auto pos = ftell(file);
    fseek(file, 0, SEEK_END);
    auto size = ftell(file);
    fseek(file, pos, SEEK_SET);
    return size;
  }
};
struct HostStorage {
  bool openFileForRead(const char*, const std::string& p, HalFile& f) { return f.open(p, "rb"); }
  bool openFileForWrite(const char*, const std::string& p, HalFile& f) { return f.open(p, "wb+"); }
  bool exists(const char* p) { return std::filesystem::exists(p); }
  bool remove(const char* p) { return std::filesystem::remove(p); }
  HalFile open(const char* p, int) {
    HalFile f;
    f.open(p, "wb+");
    return f;
  }
};
inline HostStorage Storage;
