#pragma once

#include "AnnotationEntry.h"
#include "activities/ActivityResult.h"

// Preserve the same exact-offset / XPath fallback contract as bookmark navigation.
inline ProgressChangeResult annotationProgress(const AnnotationEntry& entry) {
  const auto& location = entry.location;
  ProgressChangeResult progress;
  progress.spineIndex = location.computedSpineIndex;
  progress.page = location.computedChapterProgress;
  progress.totalPages = location.computedChapterPageCount;
  progress.xpath = location.xpath;
  progress.percentage = location.percentage;
  progress.hasSavedProgress = true;
  progress.hasVisibleTextOffset = location.hasVisibleTextOffset;
  progress.visibleTextOffset = location.visibleTextOffset;
  return progress;
}
