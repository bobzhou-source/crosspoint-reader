#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Dictionary.h"

// Looks one word up across every installed dictionary, so an English word
// still finds the English dictionary while CEDICT is the selected one (and a
// Chinese word finds CEDICT while English is selected). The selected folder is
// tried first, then the others: those whose name suits the query's script
// (cedict/cjk/zh/chinese for Han queries, english/eng/en* otherwise) ahead of
// the rest. The dictionary that answered stays open, so the definition view
// and the next lookup reuse it.
class DictionaryRouter {
 public:
  // Why lookup() found nothing. Ordered by precedence: the first real error
  // (index build or definition read) beats a plain miss, which beats a folder
  // that would not open.
  enum class Miss : uint8_t {
    NoDictionary,  // nothing installed
    OpenFailed,    // no listed folder opened
    NotFound,      // searched, genuinely absent
    IndexLowMemory,
    IndexReadError,
    Decompress,
    LowMemory,
    ReadError,
  };

  static bool suitsScript(const char* name, const bool cjk) {
    std::string lower(name ? name : "");
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto has = [&lower](const char* part) { return lower.find(part) != std::string::npos; };
    if (cjk) return has("cedict") || has("cjk") || has("zh") || has("chinese");
    return has("eng") || lower.compare(0, 2, "en") == 0;
  }

  // Lookup order: `selected` (when set), then the other installed names, the
  // ones that suit the query's script first, each group in installed order.
  static void order(const std::vector<std::string>& installed, const char* selected, const bool cjk,
                    std::vector<std::string>& out) {
    out.clear();
    const bool hasSelected = selected && selected[0];
    if (hasSelected) out.emplace_back(selected);
    for (const bool suits : {true, false}) {
      for (const auto& name : installed) {
        if ((hasSelected && name == selected) || suitsScript(name.c_str(), cjk) != suits) continue;
        out.push_back(name);
      }
    }
  }

  // Tries `names` in order until one has the word. A CJK query passes its run
  // (and the run one character earlier, see Dictionary::lookupCjk); a Latin
  // query passes word with an empty cjkRun. beforeIndex() runs before a
  // dictionary's slow first-use index build, which feeds indexYield (may be
  // null) every ~64KB. On a hit, dictionary() is the one that answered.
  template <typename BeforeIndex>
  bool lookup(const std::vector<std::string>& names, const char* word, const char* cjkRun, const char* previousRun,
              BeforeIndex&& beforeIndex, void (*indexYield)(void*), std::string& definition, std::string& headword,
              uint8_t& matchMask, bool& usedPrevious, Miss& miss) {
    miss = Miss::NoDictionary;
    usedPrevious = false;
    const auto note = [&miss](const Miss m) {
      if (miss <= Miss::NotFound && m > miss) miss = m;
    };
    for (const auto& name : names) {
      if (name != openName) {
        openName = name;
        openOk = dict.open(name.c_str());
        needsIndex = openOk && dict.needsIndex();
      }
      if (!openOk) {
        note(Miss::OpenFailed);
        continue;
      }
      if (needsIndex) {
        beforeIndex();
        Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
        needsIndex = !dict.buildIndex(indexYield, nullptr, &indexResult);
        if (needsIndex) {
          note(indexResult == Dictionary::IndexResult::LowMemory ? Miss::IndexLowMemory : Miss::IndexReadError);
          continue;
        }
      }
      Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
      const bool found =
          cjkRun[0] ? dict.lookupCjk(cjkRun, definition, headword, matchMask, &result, previousRun, &usedPrevious)
                    : dict.lookup(word, definition, headword, &result);
      if (found) return true;
      switch (result) {
        case Dictionary::LookupResult::Decompress:
          note(Miss::Decompress);
          break;
        case Dictionary::LookupResult::LowMemory:
          note(Miss::LowMemory);
          break;
        case Dictionary::LookupResult::ReadError:
          note(Miss::ReadError);
          break;
        default:
          note(Miss::NotFound);
          break;
      }
    }
    return false;
  }

  Dictionary& dictionary() { return dict; }

 private:
  Dictionary dict;
  std::string openName;
  bool openOk = false;
  bool needsIndex = false;
};
