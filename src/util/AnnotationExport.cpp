#include "AnnotationExport.h"

#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "AnnotationStore.h"

namespace {
// A small streaming writer avoids assembling the document in RAM or doing one
// SD transaction per escaped character. It lives on the heap (256-byte buffer).
class MarkdownWriter {
 public:
  explicit MarkdownWriter(HalFile& file) : file(file) {}
  void text(const std::string_view text) {
    for (const char c : text) character(c);
  }
  void escaped(const std::string_view text) {
    for (const unsigned char c : text) {
      if (c == '&') this->text("&amp;");
      else if (c == '<') this->text("&lt;");
      else if (c == '>') this->text("&gt;");
      else if (c <= 0x20 || c == 0x7f) character(' ');
      else {
        if (strchr("\\`*_{}[]()#+-.!|~", c)) character('\\');
        character(static_cast<char>(c));
      }
    }
  }
  void field(const char* key, const std::string_view value) {
    text("- ");
    text(key);
    text(": ");
    escaped(value);
    text("\n");
  }
  bool flush() {
    if (used && good) good = file.write(reinterpret_cast<const uint8_t*>(buffer), used) == used;
    used = 0;
    return good;
  }

 private:
  void character(const char c) {
    if (!good) return;
    buffer[used++] = c;
    if (used == sizeof(buffer)) flush();
  }
  HalFile& file;
  char buffer[256];
  size_t used = 0;
  bool good = true;
};

std::string exportPath(const std::string& bookPath) {
  const size_t slash = bookPath.find_last_of("/\\");
  std::string_view stem(bookPath);
  if (slash != std::string::npos) stem.remove_prefix(slash + 1);
  const size_t dot = stem.find_last_of('.');
  if (dot != std::string_view::npos && dot > 0) stem = stem.substr(0, dot);
  std::string safe;
  safe.reserve(64);
  for (const unsigned char c : stem) {
    if (safe.size() >= 48) break;
    // ASCII-only names are portable across FAT and desktop sync tools. A stable
    // source suffix keeps same-named books in different folders separate.
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
      safe.push_back(static_cast<char>(c));
    } else if (!safe.empty() && safe.back() != '_') {
      safe.push_back('_');
    }
  }
  while (!safe.empty() && safe.back() == '_') safe.pop_back();
  if (safe.empty()) safe = "book";
  return "/annotations/" + safe + "-" + AnnotationStore::sourceKey(bookPath) + ".md";
}

bool writeEntry(MarkdownWriter& writer, const AnnotationEntry& entry) {
  char value[80];
  snprintf(value, sizeof(value), "\n## %u\n\n", static_cast<unsigned>(entry.id));
  writer.text(value);
  // These English keys are the stable Markdown interchange format, independent
  // of the device's current UI locale.
  writer.field("Title", entry.sourceTitle);
  writer.field("Source", entry.sourcePath);
  static constexpr const char* KINDS[] = {"Quote", "Question", "Review later"};
  writer.field("Type", KINDS[static_cast<uint8_t>(entry.kind)]);
  writer.field("Chapter", entry.chapterTitle.empty() ? entry.chapterHref : entry.chapterTitle);
  writer.field("Chapter path", entry.chapterHref);
  snprintf(value, sizeof(value), "%u", static_cast<unsigned>(entry.location.computedSpineIndex));
  writer.field("Spine index", value);
  snprintf(value, sizeof(value), "%.2f%%", static_cast<double>(entry.location.percentage) * 100.0);
  writer.field("Progress", value);
  writer.field("XPath", entry.location.xpath);
  if (entry.location.hasVisibleTextOffset) {
    snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(entry.location.visibleTextOffset));
    writer.field("Visible text offset", value);
  }
  if (entry.location.computedChapterPageCount > 0) {
    snprintf(value, sizeof(value), "%u/%u", static_cast<unsigned>(entry.location.computedChapterProgress) + 1,
             static_cast<unsigned>(entry.location.computedChapterPageCount));
    writer.field("Page at capture", value);
  }
  if (entry.createdAt) {
    snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(entry.createdAt));
    writer.field("Created (Unix UTC)", value);
  }
  writer.field("Text kind", entry.pageContext ? "Page context" : "Selected excerpt");
  writer.text("\n> ");
  writer.escaped(entry.excerpt);
  writer.text("\n");
  if (!entry.note.empty()) {
    writer.text("\n**Note:** ");
    writer.escaped(entry.note);
    writer.text("\n");
  }
  return writer.flush();
}

bool commitExport(const std::string& destination, const std::string& temporary) {
  // Preserve the last complete export until its replacement has been written.
  const std::string backup = destination + ".bak";
  if (!Storage.exists(destination.c_str()) && Storage.exists(backup.c_str()) &&
      !Storage.rename(backup.c_str(), destination.c_str())) {
    Storage.remove(temporary.c_str());
    LOG_ERR("ANN", "Could not recover previous export");
    return false;
  }
  const bool replacing = Storage.exists(destination.c_str());
  if (replacing && ((Storage.exists(backup.c_str()) && !Storage.remove(backup.c_str())) ||
                    !Storage.rename(destination.c_str(), backup.c_str()))) {
    Storage.remove(temporary.c_str());
    LOG_ERR("ANN", "Could not preserve previous export");
    return false;
  }
  if (!Storage.rename(temporary.c_str(), destination.c_str())) {
    if (replacing) Storage.rename(backup.c_str(), destination.c_str());
    Storage.remove(temporary.c_str());
    LOG_ERR("ANN", "Could not commit annotation export");
    return false;
  }
  if (replacing && !Storage.remove(backup.c_str())) LOG_ERR("ANN", "Could not remove old export backup");
  return true;
}
}  // namespace

std::string AnnotationExport::pageContext(Section& section) {
  std::string text;
  text.reserve(AnnotationEntry::MAX_EXCERPT_BYTES);
  const auto page = section.loadPage(section.currentPage);
  if (!page) return text;
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    if (!line.getBlock()) continue;
    const auto& block = *line.getBlock();
    for (uint16_t i = 0; i < block.wordCount(); ++i) {
      if (text.size() >= AnnotationEntry::MAX_EXCERPT_BYTES) return annotationText(text, AnnotationEntry::MAX_EXCERPT_BYTES);
      if (!text.empty()) text.push_back(' ');
      const char* word = block.wordText(i);
      const size_t remaining = AnnotationEntry::MAX_EXCERPT_BYTES - text.size();
      text.append(word, strnlen(word, remaining));
    }
  }
  return annotationText(text, AnnotationEntry::MAX_EXCERPT_BYTES);
}

bool AnnotationExport::capture(AnnotationEntry& out, const Epub& epub, const std::string& bookPath,
                               const BookmarkEntry& location, const std::string_view context,
                               const std::string_view selectedExcerpt) {
  if (bookPath.empty() || bookPath.size() > AnnotationEntry::MAX_PATH_BYTES || location.xpath.empty() ||
      location.xpath.size() > AnnotationEntry::MAX_PATH_BYTES || location.computedSpineIndex >= epub.getSpineItemsCount() ||
      !std::isfinite(location.percentage)) {
    LOG_ERR("ANN", "Cannot capture annotation without a valid source and location");
    return false;
  }
  out.id = 0;
  out.sourcePath = bookPath;
  out.sourceTitle = annotationText(epub.getTitle(), AnnotationEntry::MAX_TITLE_BYTES);
  if (out.sourceTitle.empty()) out.sourceTitle = annotationText(bookPath, AnnotationEntry::MAX_TITLE_BYTES);
  out.chapterHref = epub.getSpineItem(location.computedSpineIndex).href;
  if (out.chapterHref.empty() || out.chapterHref.size() > AnnotationEntry::MAX_PATH_BYTES) {
    LOG_ERR("ANN", "Cannot capture an oversized or missing chapter path");
    return false;
  }
  const int tocIndex = epub.getTocIndexForSpineIndex(location.computedSpineIndex);
  out.chapterTitle = tocIndex >= 0 ? annotationText(epub.getTocItem(tocIndex).title, AnnotationEntry::MAX_TITLE_BYTES) : "";
  out.location.xpath = location.xpath;
  out.location.percentage = std::clamp(location.percentage, 0.0f, 1.0f);
  out.location.computedSpineIndex = location.computedSpineIndex;
  out.location.computedChapterPageCount = location.computedChapterPageCount;
  out.location.computedChapterProgress = location.computedChapterProgress;
  out.location.hasVisibleTextOffset = location.hasVisibleTextOffset;
  out.location.visibleTextOffset = location.visibleTextOffset;
  out.location.summary.clear();
  out.location.name.clear();
  out.excerpt = annotationText(selectedExcerpt, AnnotationEntry::MAX_EXCERPT_BYTES);
  out.pageContext = out.excerpt.empty();
  if (out.pageContext) out.excerpt = annotationText(context, AnnotationEntry::MAX_EXCERPT_BYTES);
  out.note.clear();
  out.kind = AnnotationKind::Quote;
  // Never represent uptime/the default 1970 clock as a real capture timestamp.
  const time_t now = time(nullptr);
  out.createdAt = now >= 1577836800 ? static_cast<uint64_t>(now) : 0;
  if (out.excerpt.empty()) {
    LOG_ERR("ANN", "No visible text available for annotation capture");
    return false;
  }
  return true;
}

bool AnnotationExport::writeMarkdown(const std::string& bookPath, const std::string& title, std::string& outputPath) {
  // At most ~16 KiB of previews; full annotation records are loaded one at a time.
  auto entries = makeUniqueNoThrow<AnnotationSummary[]>(AnnotationStore::MAX_ENTRIES);
  if (!entries) {
    LOG_ERR("ANN", "OOM: annotation export index");
    return false;
  }
  size_t count = 0;
  if (AnnotationStore::list(bookPath, entries.get(), AnnotationStore::MAX_ENTRIES, count) != AnnotationStore::Result::Success) {
    return false;
  }
  return writeMarkdown(bookPath, title, entries.get(), count, outputPath);
}

bool AnnotationExport::writeMarkdown(const std::string& bookPath, const std::string& title,
                                      const AnnotationSummary* sortedEntries, const size_t count,
                                      std::string& outputPath) {
  if (count > AnnotationStore::MAX_ENTRIES || (count && !sortedEntries) || bookPath.empty() ||
      bookPath.size() > AnnotationEntry::MAX_PATH_BYTES || !Storage.mkdir("/annotations")) {
    LOG_ERR("ANN", "Cannot prepare annotation export");
    return false;
  }
  const std::string destination = exportPath(bookPath);
  const std::string temporary = destination + ".tmp";
  bool written = false;
  {
    HalFile file;
    if (!Storage.openFileForWrite("ANN", temporary, file)) return false;
    auto writer = makeUniqueNoThrow<MarkdownWriter>(file);
    auto entry = makeUniqueNoThrow<AnnotationEntry>();
    if (!writer || !entry) {
      LOG_ERR("ANN", "OOM: annotation export buffers");
    } else {
      writer->text("# ");
      writer->escaped(annotationText(title.empty() ? bookPath : title, AnnotationEntry::MAX_TITLE_BYTES));
      writer->text("\n\n");
      writer->field("Source", bookPath);
      written = writer->flush();
      for (size_t i = 0; written && i < count; ++i) {
        if (AnnotationStore::load(bookPath, sortedEntries[i].id, *entry) != AnnotationStore::Result::Success) {
          written = false;
          break;
        }
        written = writeEntry(*writer, *entry);
      }
    }
    file.flush();
    written = file.close() && written;  // Complete before rename/remove.
  }
  if (!written) {
    Storage.remove(temporary.c_str());
    LOG_ERR("ANN", "Annotation export write failed");
    return false;
  }

  if (!commitExport(destination, temporary)) return false;
  outputPath = destination;
  return true;
}

bool AnnotationExport::writeInboxMarkdown(std::string& outputPath) {
  if (!Storage.mkdir("/annotations")) {
    LOG_ERR("ANN", "Cannot prepare inbox export");
    return false;
  }
  const std::string destination = "/annotations/reading-inbox.md";
  const std::string temporary = destination + ".tmp";
  bool written = false;
  {
    HalFile file;
    if (!Storage.openFileForWrite("ANN", temporary, file)) return false;
    auto writer = makeUniqueNoThrow<MarkdownWriter>(file);
    if (!writer) {
      LOG_ERR("ANN", "OOM: inbox export buffer");
    } else {
      writer->text("# Reading inbox\n\n- Scope: All sources\n");
      written = writer->flush() &&
                AnnotationStore::visitAll(
                    [](const AnnotationEntry& entry, void* context) {
                      return writeEntry(*static_cast<MarkdownWriter*>(context), entry)
                                 ? AnnotationStore::Result::Success
                                 : AnnotationStore::Result::IoError;
                    },
                    writer.get()) == AnnotationStore::Result::Success;
    }
    file.flush();
    written = file.close() && written;
  }
  if (!written) {
    Storage.remove(temporary.c_str());
    LOG_ERR("ANN", "Inbox export write failed");
    return false;
  }
  if (!commitExport(destination, temporary)) return false;
  outputPath = destination;
  return true;
}
