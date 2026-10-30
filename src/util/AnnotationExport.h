#pragma once

#include <string>
#include <string_view>

#include "AnnotationEntry.h"

class Epub;
class Section;

namespace AnnotationExport {
// Reads only the current page, retaining at most MAX_EXCERPT_BYTES. Caller holds
// the reader RenderLock while accessing Section, just as for bookmark capture.
std::string pageContext(Section& section);

// No disk writes. Pass the original bookPath, not a generated EPUB/cache path.
// location carries the same stable anchor and reflow hints as a BookmarkEntry.
// An empty selection uses bounded page context, explicitly labelled as such.
bool capture(AnnotationEntry& out, const Epub& epub, const std::string& bookPath, const BookmarkEntry& location,
             std::string_view pageContext, std::string_view selectedExcerpt = {});

// Exports all annotations in reading order; no changing export-time timestamp.
// outputPath is assigned only after the complete Markdown file is committed.
bool writeMarkdown(const std::string& bookPath, const std::string& title, std::string& outputPath);
// Reuses the already sorted, complete index from AnnotationStore::list, avoiding
// a second list allocation while the annotation activity is open.
bool writeMarkdown(const std::string& bookPath, const std::string& title, const AnnotationSummary* sortedEntries,
                   size_t count, std::string& outputPath);

// Streams every native annotation across all books, independent of inbox paging.
// Each record retains its original Source and book-local ID for companion sync.
bool writeInboxMarkdown(std::string& outputPath);
}  // namespace AnnotationExport
