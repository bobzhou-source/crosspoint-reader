#pragma once
#include <cstring>
#include <map>

#include "Arduino.h"
struct HostFile {
  const std::string* data = nullptr;
  explicit operator bool() const { return data != nullptr; }
  size_t size() const { return data ? data->size() : 0; }
  int read(void* target, size_t length) {
    if (!data || length > data->size()) return -1;
    memcpy(target, data->data(), length);
    return length;
  }
};
struct HostStorage {
  std::map<std::string, std::string> files;
  void mkdir(const char*) {}
  bool writeFile(const char* path, const String& raw) {
    files[path] = raw;
    return true;
  }
  bool exists(const char* path) { return files.count(path); }
  String readFile(const char* path) { return files[path]; }
  HostFile open(const char* path) {
    auto i = files.find(path);
    return {i == files.end() ? nullptr : &i->second};
  }
};
inline HostStorage Storage;
