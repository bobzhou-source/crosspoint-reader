#pragma once

#include "AnnotationEntry.h"

class AnnotationStore {
 public:
  static constexpr size_t MAX_ENTRIES = 128;
  enum class Result { Success, NotFound, LimitReached, InvalidData, IoError };

  // One bounded JSON file per annotation; source identity is never derived from a
  // temporary converted EPUB path. All callers pass the original bookPath.
  static Result load(const std::string& bookPath, uint16_t id, AnnotationEntry& entry);
  static Result save(AnnotationEntry& entry);
  static Result remove(const std::string& bookPath, uint16_t id);
  // Missing directory is an empty list. Corrupt/unreadable records fail the whole
  // operation, so export never silently replaces a complete export with a subset.
  static Result list(const std::string& bookPath, AnnotationSummary* entries, size_t capacity, size_t& count);

  // Cross-book access uses the existing source key and record identity, not a
  // second metadata store. The visitor borrows one reused entry at a time.
  static Result loadFromSourceKey(std::string_view key, uint16_t id, AnnotationEntry& entry);
  using Visitor = Result (*)(const AnnotationEntry& entry, void* context);
  static Result visitAll(Visitor visitor, void* context);

  // Stable FNV-1a key, also used to disambiguate exported book filenames.
  static std::string sourceKey(std::string_view bookPath);
};
