#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace FsHelpers {
inline bool checkFileExtension(std::string_view path, const char* extension) {
  const std::string_view ext(extension);
  if (path.size() < ext.size()) return false;
  const size_t offset = path.size() - ext.size();
  for (size_t i = 0; i < ext.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(path[offset + i])) !=
        std::tolower(static_cast<unsigned char>(ext[i])))
      return false;
  }
  return true;
}
inline bool hasEpubExtension(std::string_view path) { return checkFileExtension(path, ".epub"); }
inline bool hasTxtExtension(std::string_view path) { return checkFileExtension(path, ".txt"); }
inline bool hasMarkdownExtension(std::string_view path) {
  return checkFileExtension(path, ".md") || checkFileExtension(path, ".markdown");
}
inline bool hasFb2Extension(std::string_view path) { return checkFileExtension(path, ".fb2"); }
inline bool hasHtmlExtension(std::string_view path) {
  return checkFileExtension(path, ".html") || checkFileExtension(path, ".htm");
}
inline bool hasXtcExtension(std::string_view path) {
  return checkFileExtension(path, ".xtc") || checkFileExtension(path, ".xtch");
}
}  // namespace FsHelpers
