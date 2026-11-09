#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The lookup popup's sections (Definition, Story, Related, Riff), each in a
// compact and a longer form, and the pure layout logic behind its
// [All | Def | Story | Related | Riff] tabs and [Compact | Longer] toggle.
// No renderer here, so the host tests cover it directly.
namespace DictionaryLookup {

enum class Section : uint8_t { All, Def, Story, Related, Riff };
constexpr size_t SECTION_COUNT = 5;

// Compact definitions keep this many leading lines (the best sense is ranked
// first) and bytes.
constexpr size_t COMPACT_DEFINITION_LINES = 6;
constexpr size_t COMPACT_DEFINITION_BYTES = 480;

// One composed line's role, for pagination.
enum class LineKind : uint8_t { Text, Blank, Header };

struct LookupSections {
  std::string defShort, defLong;
  std::string storyShort, storyLong;
  std::string relatedShort, relatedLong;
  std::string riffShort, riffLong;

  // A section's text; the longer form falls back to the compact one. Empty
  // for All.
  const std::string& text(const Section section, const bool longer) const {
    static const std::string empty;
    const std::string* shortText = nullptr;
    const std::string* longText = nullptr;
    switch (section) {
      case Section::Def:
        shortText = &defShort;
        longText = &defLong;
        break;
      case Section::Story:
        shortText = &storyShort;
        longText = &storyLong;
        break;
      case Section::Related:
        shortText = &relatedShort;
        longText = &relatedLong;
        break;
      case Section::Riff:
        shortText = &riffShort;
        longText = &riffLong;
        break;
      case Section::All:
      default:
        return empty;
    }
    if (longer && !longText->empty()) return *longText;
    return shortText->empty() ? *longText : *shortText;
  }

  bool has(const Section section) const {
    if (section == Section::All) return true;
    return !text(section, false).empty();
  }

  // Story, Related or Riff present: worth the tabbed view.
  bool hasExtras() const { return has(Section::Story) || has(Section::Related) || has(Section::Riff); }
};

inline const char* sectionLabel(const Section section) {
  switch (section) {
    case Section::Def:
      return "Def";
    case Section::Story:
      return "Story";
    case Section::Related:
      return "Related";
    case Section::Riff:
      return "Riff";
    case Section::All:
    default:
      return "All";
  }
}

inline const char* sectionHeader(const Section section) {
  switch (section) {
    case Section::Def:
      return "DEFINITION";
    case Section::Story:
      return "STORY SO FAR";
    case Section::Related:
      return "RELATED PASSAGES";
    case Section::Riff:
      return "MARGIN RIFF";
    case Section::All:
    default:
      return "";
  }
}

// The next tab after `current` in `direction` (+1 / -1) that has text,
// wrapping around; All always has.
inline Section nextSection(const LookupSections& sections, const Section current, const int direction) {
  const int step = direction < 0 ? static_cast<int>(SECTION_COUNT) - 1 : 1;
  int index = static_cast<int>(current);
  for (size_t tries = 0; tries < SECTION_COUNT; ++tries) {
    index = (index + step) % static_cast<int>(SECTION_COUNT);
    const Section candidate = static_cast<Section>(index);
    if (sections.has(candidate)) return candidate;
  }
  return Section::All;
}

// The leading whole lines of `text`, at most `maxLines` and `maxBytes`, with
// "..." on its own line when anything was left out. A first line longer than
// `maxBytes` is cut on a word (else code point) boundary.
inline std::string compactDefinition(std::string_view text, const size_t maxLines = COMPACT_DEFINITION_LINES,
                                     const size_t maxBytes = COMPACT_DEFINITION_BYTES) {
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.remove_suffix(1);
  size_t end = 0;
  size_t lines = 0;
  while (end < text.size() && lines < maxLines) {
    size_t next = text.find('\n', end);
    next = next == std::string_view::npos ? text.size() : next;
    if (next > maxBytes) break;
    end = next;
    ++lines;
    if (end < text.size()) ++end;  // past the '\n'
  }
  if (end >= text.size()) return std::string(text);
  if (end == 0) {
    // The first line alone is too long: cut inside it.
    size_t cut = std::min(maxBytes, text.size());
    while (cut > 0 && (static_cast<uint8_t>(text[cut]) & 0xC0) == 0x80) --cut;
    const size_t space = text.substr(0, cut).rfind(' ');
    if (space != std::string_view::npos && space * 3 >= cut * 2) cut = space;
    return std::string(text.substr(0, cut)) + "...";
  }
  std::string out(text.substr(0, end));
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out + "\n...";
}

// The body for `section`: each present section as a header line (its offset
// in the result goes to `headerOffsets`) followed by its text, sections
// separated by a blank line. A single tab still gets its header.
inline std::string composeSections(const LookupSections& sections, const Section section, const bool longer,
                                   std::vector<uint32_t>& headerOffsets) {
  headerOffsets.clear();
  std::string out;
  const auto add = [&](const Section part) {
    const std::string& body = sections.text(part, longer);
    if (body.empty()) return;
    if (!out.empty()) out += "\n\n";
    headerOffsets.push_back(static_cast<uint32_t>(out.size()));
    out += sectionHeader(part);
    out += '\n';
    out += body;
  };
  if (section == Section::All) {
    for (const Section part : {Section::Def, Section::Story, Section::Related, Section::Riff}) add(part);
  } else {
    add(section);
  }
  return out;
}

// First line of each page: at most `linesPerPage` lines a page, blank lines
// never open a page, and a header never closes one (it moves to the next
// page with its text). Always at least one page (starting at line 0).
inline std::vector<uint32_t> paginateLines(const std::vector<LineKind>& kinds, const int linesPerPage) {
  std::vector<uint32_t> starts;
  const size_t perPage = linesPerPage > 0 ? static_cast<size_t>(linesPerPage) : 1;
  size_t i = 0;
  while (i < kinds.size()) {
    while (i < kinds.size() && kinds[i] == LineKind::Blank) ++i;
    if (i >= kinds.size()) break;
    starts.push_back(static_cast<uint32_t>(i));
    size_t end = std::min(i + perPage, kinds.size());
    // Drop trailing blanks and headers (with the blanks before them) while
    // the page keeps at least one line.
    while (end < kinds.size() && end - 1 > i &&
           (kinds[end - 1] == LineKind::Header || kinds[end - 1] == LineKind::Blank)) {
      --end;
    }
    i = end;
  }
  if (starts.empty()) starts.push_back(0);
  return starts;
}

}  // namespace DictionaryLookup
