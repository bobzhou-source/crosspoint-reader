#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "BookmarkEntry.h"

enum class AnnotationKind : uint8_t { Quote = 0, Question = 1, ReviewLater = 2 };

struct AnnotationEntry {
  static constexpr size_t MAX_PATH_BYTES = 512;
  static constexpr size_t MAX_TITLE_BYTES = 192;
  static constexpr size_t MAX_EXCERPT_BYTES = 768;
  static constexpr size_t MAX_NOTE_BYTES = 256;

  uint16_t id = 0;  // Zero until first save; stable within this source book.
  std::string sourcePath;
  AnnotationKind kind = AnnotationKind::Quote;
  std::string sourceTitle;
  std::string chapterTitle;
  std::string chapterHref;
  BookmarkEntry location;
  std::string excerpt;
  std::string note;
  bool pageContext = true;  // False only when the caller supplies an explicit selection.
  uint64_t createdAt = 0;   // UTC Unix seconds; zero when the device clock is unset.
};

// The list keeps only these previews in memory, not every excerpt/note/anchor.
struct AnnotationSummary {
  uint16_t id = 0;
  AnnotationKind kind = AnnotationKind::Quote;
  uint16_t spineIndex = 0;
  uint32_t visibleTextOffset = 0;
  bool hasVisibleTextOffset = false;
  float percentage = 0;
  char label[97] = {};
};

// Collapses whitespace and truncates at a UTF-8 boundary; cold capture/edit paths only.
std::string annotationText(std::string_view text, size_t maxBytes);
