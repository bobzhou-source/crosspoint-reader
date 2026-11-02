#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

#include "BenchmarkBuffer.h"
#include "NavFeatures.h"
#include "NavScript.h"

using namespace benchmark::nav;

namespace {

class FakeProbe : public FeatureProbe {
 public:
  std::set<std::string> files;
  uint32_t recents = 0;
  bool dictionary = false;
  bool hasTouch = false;
  bool opds = false;
  bool exists(const char* path) const override { return files.count(path) != 0; }
  uint32_t recentCount() const override { return recents; }
  bool dictionarySelected() const override { return dictionary; }
  bool touch() const override { return hasTouch; }
  bool opdsConfigured() const override { return opds; }
  bool idle = true;
  bool networkIdle() const override { return idle; }
};

bool has(const FakeProbe& probe, const char* name) { return featureAvailable(probe, benchmark::id(name)); }

}  // namespace

TEST(NavFeatures, FirmwareFeaturesAreAlwaysAvailable) {
  FakeProbe probe;
  for (const char* name : {"stats", "darkmenus", "night", "wifiui", "cjklookup"}) EXPECT_TRUE(has(probe, name)) << name;
}

TEST(NavFeatures, FixtureFeaturesFollowTheirFileOnTheCard) {
  FakeProbe probe;
  const struct {
    const char* name;
    const char* path;
  } fixtures[] = {{"fixture", "/.crosspoint/nav-fixtures/navigation.epub"},
                  {"probebook", "/navfeat/probebook.epub"},
                  {"bigepub", "/navfeat/spinebook.epub"},
                  {"groupbook", "/navfeat/groupbook.epub"},
                  {"txt", "/navfeat/novelbook.txt"},
                  {"navdict", "/dictionaries/navigation"},
                  {"cedict", "/dictionaries/cedict/cedict.ifo"}};
  for (const auto& f : fixtures) {
    EXPECT_FALSE(has(probe, f.name)) << f.name;
    probe.files.insert(f.path);
    EXPECT_TRUE(has(probe, f.name)) << f.name;
  }
}

TEST(NavFeatures, AFixtureFeatureDoesNotBorrowAnotherFixture) {
  FakeProbe probe;
  probe.files.insert("/navfeat/probebook.epub");
  EXPECT_TRUE(has(probe, "probebook"));
  EXPECT_FALSE(has(probe, "bigepub"));
  EXPECT_FALSE(has(probe, "txt"));
  EXPECT_FALSE(has(probe, "cedict"));
}

TEST(NavFeatures, RecentAndNorecentAreComplementary) {
  FakeProbe probe;
  EXPECT_FALSE(has(probe, "recent"));
  EXPECT_TRUE(has(probe, "norecent"));
  probe.recents = 3;
  EXPECT_TRUE(has(probe, "recent"));
  EXPECT_FALSE(has(probe, "norecent"));
}

TEST(NavFeatures, SyncSafeNeedsAConfigurationNoSavedNetworkAndNoLiveNetworkPath) {
  FakeProbe probe;
  EXPECT_FALSE(has(probe, "syncsafe"));  // nothing configured: a Sync now press would start nothing
  probe.files.insert("/.crosspoint/reader-sync.json");
  EXPECT_TRUE(has(probe, "syncsafe"));
  probe.files.insert("/.crosspoint/wifi.json");
  EXPECT_FALSE(has(probe, "syncsafe"));  // a saved network file
  probe.files.erase("/.crosspoint/wifi.json");
  probe.idle = false;
  EXPECT_FALSE(has(probe, "syncsafe"));  // a credential in RAM, or a running or connected radio
  probe.idle = true;
  EXPECT_TRUE(has(probe, "syncsafe"));
}

TEST(NavFeatures, SyncSafeAcceptsTheBackupConfiguration) {
  FakeProbe probe;
  probe.files.insert("/.crosspoint/reader-sync.json.bak");
  EXPECT_TRUE(has(probe, "syncsafe"));
}

TEST(NavFeatures, ColdDictionaryCopiesAreSeparateFixtures) {
  FakeProbe probe;
  probe.files.insert("/navfeat/probebook.epub");
  for (const char* name : {"coldfix1", "coldfix2", "coldfix3"}) EXPECT_FALSE(has(probe, name)) << name;
  probe.files.insert("/dictionaries/navcold2");
  EXPECT_FALSE(has(probe, "coldfix1"));
  EXPECT_TRUE(has(probe, "coldfix2"));
  EXPECT_FALSE(has(probe, "coldfix3"));
}

TEST(NavFeatures, DictionaryAndTouchFollowTheDeviceState) {
  FakeProbe probe;
  EXPECT_FALSE(has(probe, "dictionary"));
  EXPECT_FALSE(has(probe, "touch"));
  probe.dictionary = true;
  probe.hasTouch = true;
  EXPECT_TRUE(has(probe, "dictionary"));
  EXPECT_TRUE(has(probe, "touch"));
}

TEST(NavFeatures, OpdsAndNoOpdsAreComplementary) {
  FakeProbe probe;
  EXPECT_FALSE(has(probe, "opds"));
  EXPECT_TRUE(has(probe, "noopds"));
  probe.opds = true;
  EXPECT_TRUE(has(probe, "opds"));
  EXPECT_FALSE(has(probe, "noopds"));
}

TEST(NavFeatures, ReaderFixAnyBookOnTheCardOrNone) {
  FakeProbe probe;
  EXPECT_FALSE(has(probe, "readerfix"));
  EXPECT_TRUE(has(probe, "noreaderfix"));
  probe.files.insert("/dictionaries/navigation");  // a dictionary is not a book
  EXPECT_FALSE(has(probe, "readerfix"));
  for (const char* book : {"/navfeat/novelbook.txt", "/.crosspoint/nav-fixtures/navigation.epub"}) {
    FakeProbe one;
    one.files.insert(book);
    EXPECT_TRUE(has(one, "readerfix")) << book;
    EXPECT_FALSE(has(one, "noreaderfix")) << book;
  }
}

TEST(NavFeatures, ABookWithItsDictionaryNeedsBothFilesOnTheCard) {
  FakeProbe probe;
  probe.files.insert("/navfeat/probebook.epub");
  EXPECT_FALSE(has(probe, "dictfix"));
  EXPECT_FALSE(has(probe, "cedictfix"));
  probe.files.insert("/dictionaries/navigation");
  EXPECT_TRUE(has(probe, "dictfix"));
  EXPECT_FALSE(has(probe, "cedictfix"));
  probe.files.erase("/navfeat/probebook.epub");
  EXPECT_FALSE(has(probe, "dictfix"));  // the dictionary alone is not enough
}

TEST(NavFeatures, UnknownFeaturesAreUnavailableNeverPassed) {
  FakeProbe probe;
  probe.files.insert("/navfeat/probebook.epub");
  EXPECT_FALSE(has(probe, "nosuchthing"));
  EXPECT_FALSE(has(probe, ""));
  EXPECT_FALSE(has(probe, "PROBEBOOK"));
}

TEST(NavFeatures, EveryRuleNameIsDistinct) {
  std::set<uint32_t> ids;
  for (size_t i = 0; i < FEATURE_RULE_COUNT; ++i) EXPECT_TRUE(ids.insert(benchmark::id(FEATURE_RULES[i].name)).second);
}

TEST(NavFeatures, AStepWhoseFixtureIsMissingIsSkippedNotPassed) {
  FakeProbe probe;  // empty card
  struct Adapter {
    static bool check(uint32_t id, void* ctx) { return featureAvailable(*static_cast<FakeProbe*>(ctx), id); }
  };
  Sequencer sequencer;
  ASSERT_TRUE(sequencer.begin("txt.menu guard=any key=Confirm needs=txt\n", 0, &Adapter::check, &probe));
  Observation observation;
  Command command;
  observation.nowUs = 1000;
  while (sequencer.poll(observation, command) && command.kind != Command::Kind::Finish) {
  }
  EXPECT_EQ(sequencer.summary().skipped, 1u);
  EXPECT_EQ(sequencer.summary().ok, 0u);
  EXPECT_EQ(sequencer.summary().measured, 0u);
}

// NAV_FEATURE_SCRIPT=<path> checks the staged feature script against the grammar, the card limit and the feature
// table; without it the test has nothing to read.
TEST(NavFeatureScript, TheStagedScriptParsesFitsTheCardLimitAndNamesOnlyKnownFeatures) {
  const char* path = std::getenv("NAV_FEATURE_SCRIPT");
  if (!path) GTEST_SKIP() << "NAV_FEATURE_SCRIPT not set";
  std::ifstream in(path, std::ios::binary);
  ASSERT_TRUE(in.good()) << path;
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_LE(text.size(), 24576u);  // nav_run::SCRIPT_MAX
  size_t count = 0;
  const ParseError error = validate(text.c_str(), count);
  ASSERT_EQ(error.code, ParseCode::Ok) << parseCodeName(error.code) << " line " << error.line;
  EXPECT_GT(count, 100u);

  std::set<uint32_t> known;
  std::set<std::string> fixturePaths;
  for (size_t i = 0; i < FEATURE_RULE_COUNT; ++i) {
    known.insert(benchmark::id(FEATURE_RULES[i].name));
    if (FEATURE_RULES[i].path) fixturePaths.insert(FEATURE_RULES[i].path);
  }
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    Step step;
    bool blank = false;
    ASSERT_EQ(parseLine(text.data() + pos, text.data() + end, step, blank), ParseCode::Ok);
    pos = end + 1;
    if (blank) continue;
    if (step.hasNeeds) EXPECT_TRUE(known.count(step.needsId)) << step.id;
    if (step.action == ActionVerb::Open)
      EXPECT_TRUE(fixturePaths.count(step.arg)) << step.id << " opens a book no feature checks for: " << step.arg;
  }
}
