#include "NavFeatures.h"

#include <cstring>
#include <initializer_list>

#include "BenchmarkBuffer.h"

namespace benchmark::nav {

// Firmware features have a null path and are decided in featureAvailable(); the build carries them unconditionally
// (reading statistics, the dark-menus setting, night mode, the Wi-Fi screens, CJK longest-match with Shorter/Longer).
const FeatureRule FEATURE_RULES[] = {
    {"recent", nullptr},
    {"norecent", nullptr},
    {"dictionary", nullptr},
    {"touch", nullptr},
    {"opds", nullptr},
    {"noopds", nullptr},
    {"fixture", "/.crosspoint/nav-fixtures/navigation.epub"},
    {"probebook", "/navfeat/probebook.epub"},
    {"bigepub", "/navfeat/spinebook.epub"},
    {"groupbook", "/navfeat/groupbook.epub"},
    {"txt", "/navfeat/novelbook.txt"},
    {"navdict", "/dictionaries/navigation"},
    {"cedict", "/dictionaries/cedict/cedict.ifo"},
    // A step needs the book and the dictionary together; absent either, the whole segment is skipped as one.
    {"dictfix", "/navfeat/probebook.epub", "/dictionaries/navigation"},
    {"cedictfix", "/navfeat/probebook.epub", "/dictionaries/cedict/cedict.ifo"},
    // Some reader segment runs (any book fixture is on the card), so Home was last reached from a reader.
    {"readerfix", nullptr},
    {"noreaderfix", nullptr},
    {"stats", nullptr},
    {"darkmenus", nullptr},
    {"night", nullptr},
    {"wifiui", nullptr},
    {"cjklookup", nullptr},
    // No saved Wi-Fi network: a Sync now press can only report "Waiting: no saved Wi-Fi", never reach a server.
    {"syncsafe", nullptr},
    // Three isolated copies of the navigation dictionary: the first lookup in each builds its own index sidecar, so
    // each is one cold repetition. The run never deletes a sidecar; a copy is cold only until it has been used once.
    {"coldfix1", "/navfeat/probebook.epub", "/dictionaries/navcold1"},
    {"coldfix2", "/navfeat/probebook.epub", "/dictionaries/navcold2"},
    {"coldfix3", "/navfeat/probebook.epub", "/dictionaries/navcold3"},
};
const size_t FEATURE_RULE_COUNT = sizeof(FEATURE_RULES) / sizeof(FEATURE_RULES[0]);

namespace {
bool fnvEquals(const uint32_t id, const char* name) { return benchmark::id(name) == id; }

bool anyBookFixture(const FeatureProbe& probe) {
  for (const char* book : {"/.crosspoint/nav-fixtures/navigation.epub", "/navfeat/probebook.epub",
                           "/navfeat/spinebook.epub", "/navfeat/groupbook.epub", "/navfeat/novelbook.txt"}) {
    if (probe.exists(book)) return true;
  }
  return false;
}
}  // namespace

bool featureAvailable(const FeatureProbe& probe, const uint32_t id) {
  for (size_t i = 0; i < FEATURE_RULE_COUNT; ++i) {
    const FeatureRule& rule = FEATURE_RULES[i];
    if (!fnvEquals(id, rule.name)) continue;
    if (rule.path) return probe.exists(rule.path) && (!rule.path2 || probe.exists(rule.path2));
    if (!strcmp(rule.name, "recent")) return probe.recentCount() > 0;
    if (!strcmp(rule.name, "norecent")) return probe.recentCount() == 0;
    if (!strcmp(rule.name, "dictionary")) return probe.dictionarySelected();
    if (!strcmp(rule.name, "touch")) return probe.touch();
    if (!strcmp(rule.name, "opds")) return probe.opdsConfigured();
    if (!strcmp(rule.name, "noopds")) return !probe.opdsConfigured();
    if (!strcmp(rule.name, "readerfix")) return anyBookFixture(probe);
    // A real offline Sync now needs a configured sync (else the press starts nothing), no saved network file, and no
    // network path right now: no credential in the in-memory store and no Wi-Fi interface running or connected.
    if (!strcmp(rule.name, "syncsafe"))
      return (probe.exists("/.crosspoint/reader-sync.json") || probe.exists("/.crosspoint/reader-sync.json.bak")) &&
             !probe.exists("/.crosspoint/wifi.json") && probe.networkIdle();
    if (!strcmp(rule.name, "noreaderfix")) return !anyBookFixture(probe);
    return true;  // firmware features
  }
  return false;
}

}  // namespace benchmark::nav
