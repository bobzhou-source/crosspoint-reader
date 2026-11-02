#include <Arduino.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

#include "DictZip.h"
#include "Dictionary.h"
#include "DictionaryRegistry.h"
#include "StringUtils.h"

#define CHECK(test)                                               \
  do {                                                            \
    if (!(test)) {                                                \
      std::cerr << "Failed line " << __LINE__ << ": " #test "\n"; \
      std::exit(1);                                               \
    }                                                             \
  } while (0)
namespace DictionaryRegistry {
bool resolveBasePath(const char* path, std::string& out) {
  out = path;
  return true;
}
}  // namespace DictionaryRegistry
namespace DictZip {
bool extractEntry(const char*, uint32_t, uint32_t, HalFile&, ExtractError*) { std::abort(); }
}  // namespace DictZip
void be32(std::ostream& out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) out.put(static_cast<char>(value >> shift));
}
void fixture(const std::string& stem) {
  std::vector<std::pair<std::string, std::string>> entries = {{"中", "middle"},
                                                              {"中国", "China"},
                                                              {"中国人", "Chinese person"},
                                                              {"中國", "China traditional"},
                                                              {"中华人民共和国", "PRC"},
                                                              {"一二三四五六七八", "eight"},
                                                              {"一二三四五六七八九", "nine"},
                                                              {"𠀀中", "supplementary"},
                                                              {"dog", "canine"},
                                                              {"mouse", "rodent"},
                                                              {"apple", "fruit"}};
  for (int i = 0; i < 800; ++i) entries.emplace_back("z" + std::to_string(1000 + i), "filler");
  std::sort(entries.begin(), entries.end(),
            [](auto& a, auto& b) { return StringUtils::asciiCaseCmp(a.first.c_str(), b.first.c_str()) < 0; });
  std::ofstream idx(stem + ".idx", std::ios::binary), data(stem + ".dict", std::ios::binary);
  uint32_t offset = 0, mouse = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& [word, definition] = entries[i];
    idx.write(word.c_str(), word.size() + 1);
    be32(idx, offset);
    be32(idx, definition.size());
    data << definition;
    offset += definition.size();
    if (word == "mouse") mouse = i;
  }
  std::ofstream syn(stem + ".syn", std::ios::binary);
  syn.write("mice\0", 5);
  be32(syn, mouse);
  std::ofstream(stem + ".ifo") << "StarDict's dict ifo file\nversion=2.4.2\nsametypesequence=m\n";
}
int main(int argc, char** argv) {
  const std::string path =
      (std::filesystem::temp_directory_path() /
       ("cjk-dictionary-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
          .string();
  CHECK(std::filesystem::create_directory(path));
  const std::string stem = std::string(path) + "/fixture";
  fixture(stem);
  Dictionary dict;
  CHECK(dict.open(stem.c_str()));
  CHECK(dict.needsIndex());
  CHECK(dict.buildIndex());
  CHECK(!dict.needsIndex());
  std::string definition, headword;
  uint8_t mask = 0;
  Dictionary::LookupResult result;
  CHECK(dict.lookupCjk("中国人民", definition, headword, mask, &result));
  CHECK(headword == "中国人" && definition == "Chinese person" && mask == 7 &&
        result == Dictionary::LookupResult::Found);
  CHECK(dict.lookupCjk("中华人民共和国", definition, headword, mask));
  CHECK(headword == "中华人民共和国" && mask == (1 | (1 << 6)));
  CHECK(dict.lookupCjk("中国，人", definition, headword, mask));
  CHECK(headword == "中国" && mask == 3);
  CHECK(dict.lookupCjk("中國人", definition, headword, mask));
  CHECK(headword == "中國");
  CHECK(dict.lookupCjk("一二三四五六七八九十", definition, headword, mask));
  CHECK(headword == "一二三四五六七八" && mask == 128);
  CHECK(dict.lookupCjk("𠀀中国", definition, headword, mask));
  CHECK(headword == "𠀀中" && mask == 2);
  CHECK(!dict.lookupCjk("，中国", definition, headword, mask));
  CHECK(mask == 0);
  CHECK(!dict.lookupCjk("hello", definition, headword, mask));
  CHECK(!dict.lookupCjk("\xF0\xA0", definition, headword, mask));
  CHECK(dict.lookup("DOGS!", definition, headword));
  CHECK(headword == "dog" && definition == "canine");
  CHECK(dict.lookup("MICE", definition, headword));
  CHECK(headword == "mouse" && definition == "rodent");
  ESP.heap = 4;
  CHECK(!dict.lookupCjk("中国", definition, headword, mask, &result));
  CHECK(result == Dictionary::LookupResult::LowMemory);
  ESP.heap = 1024 * 1024;
  std::filesystem::rename(stem + ".idx", stem + ".missing");
  CHECK(!dict.lookupCjk("中国", definition, headword, mask, &result));
  CHECK(result == Dictionary::LookupResult::ReadError);
  CHECK(StringUtils::asciiCaseCmp("ABC", "abc") == 0);
  CHECK(StringUtils::asciiCaseCmp("Z", "中") < 0);
  CHECK(StringUtils::asciiCaseCmp("中國", "中国") > 0);
  CHECK(StringUtils::asciiCaseCmp("中", "𠀀") < 0);
  if (argc > 1) {
    const std::string source = argv[1];
    const std::string full = std::string(path) + "/cedict";
    for (auto extension : {".idx", ".dict", ".ifo"})
      std::filesystem::create_symlink(source + extension, full + extension);
    std::ifstream index(full + ".idx", std::ios::binary);
    std::string previous, word;
    size_t count = 0;
    while (std::getline(index, word, '\0')) {
      CHECK(previous.empty() || StringUtils::asciiCaseCmp(previous.c_str(), word.c_str()) < 0);
      char suffix[8];
      index.read(suffix, 8);
      CHECK(index.gcount() == 8);
      previous = word;
      ++count;
    }
    CHECK(count > 100000);
    Dictionary real;
    CHECK(real.open(full.c_str()));
    CHECK(real.buildIndex());
    CHECK(real.lookupCjk("中国人民", definition, headword, mask));
    CHECK(headword.size() >= 6);
    CHECK(real.lookup("中国", definition, headword));
    CHECK(definition.find("China") != std::string::npos);
    CHECK(real.lookup("中國", definition, headword));
    CHECK(definition.find("China") != std::string::npos);
    CHECK(real.lookup("女", definition, headword));
    CHECK(definition.find("nǚ") != std::string::npos);
    std::cout << "Validated " << count << " UTF-8 index entries with production StringUtils::asciiCaseCmp\n";
  }
  std::filesystem::remove_all(path);
  std::cout << "CJK longest-match, boundary, eight-character cap, supplementary UTF-8, error and Latin "
               "synonym/stemming tests passed\n";
}
