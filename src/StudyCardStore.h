#pragma once

#include "AnnotationEntry.h"

struct StudyCardEntry {
  static constexpr size_t MAX_FRONT_BYTES = AnnotationEntry::MAX_NOTE_BYTES;
  uint16_t id = 0;
  std::string front;
  AnnotationEntry source;
};

struct StudyCardSummary {
  uint16_t id = 0;
  AnnotationKind kind = AnnotationKind::Quote;
  uint16_t spineIndex = 0;
  char front[97] = {};
};

class StudyCardStore {
 public:
  static constexpr size_t MAX_ENTRIES = 128;
  enum class Result { Success, NotFound, LimitReached, InvalidData, IoError };

  // Explicit actions only: snapshot a persisted annotation, never an unsaved draft.
  static Result create(const std::string& bookPath, uint16_t annotationId, const char* fallbackQuestion,
                       StudyCardEntry& card);
  static Result load(const std::string& bookPath, uint16_t id, StudyCardEntry& card);
  static Result list(const std::string& bookPath, StudyCardSummary* entries, size_t capacity, size_t& count);
  static Result remove(const std::string& bookPath, uint16_t id);
};
