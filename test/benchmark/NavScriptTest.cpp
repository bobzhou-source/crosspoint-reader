#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "BenchmarkBuffer.h"
#include "NavDefaultScript.h"
#include "NavScript.h"

namespace {
using namespace benchmark;
using namespace benchmark::nav;

struct Sink {
  std::string output;
  bool retain = true;
  static size_t write(void* context, const char* text, size_t length) {
    auto& sink = *static_cast<Sink*>(context);
    if (sink.retain) sink.output.append(text, length);
    return length;
  }
};

ParseError check(const char* script, size_t* count = nullptr) {
  size_t localCount = 0;
  const ParseError error = validate(script, localCount);
  if (count) *count = localCount;
  return error;
}

Step parse(const char* line) {
  Step step;
  bool blank = false;
  EXPECT_EQ(parseLine(line, line + strlen(line), step, blank), ParseCode::Ok) << line;
  EXPECT_FALSE(blank);
  return step;
}

// Simulated device: a clock, the observable activity, and a real RefreshProbe fed like the display hook feeds it.
struct Rig {
  Sequencer sequencer;
  RefreshProbe probe;
  uint64_t now = 1000000;
  uint32_t activity = id("Home");
  bool busy = false;
  uint32_t heapFree = 100000;
  bool syncPending = false;
  uint32_t heapMinEver = 90000;
  std::vector<Command> commands;
  std::vector<Press> injected;
  std::vector<std::string> acted;  // "verb:arg" of each software action performed
  std::vector<std::string> features;

  static bool hasFeature(uint32_t featureId, void* context) {
    for (const auto& name : static_cast<Rig*>(context)->features) {
      if (id(name.c_str()) == featureId) return true;
    }
    return false;
  }

  bool begin(const char* script) { return sequencer.begin(script, now, &Rig::hasFeature, this); }

  Observation observe() const {
    Observation observation;
    observation.nowUs = now;
    observation.activityId = activity;
    observation.busy = busy;
    observation.heapFree = heapFree;
    observation.heapMinEver = heapMinEver;
    observation.refresh = probe.snapshot();
    observation.syncPending = syncPending;
    return observation;
  }

  // Runs the sequencer until it has nothing more to say at this instant, like the device loop does.
  void drain() {
    Command command;
    for (int guard = 0; guard < 64 && sequencer.poll(observe(), command); ++guard) {
      commands.push_back(command);
      if (command.kind == Command::Kind::Inject) {
        probe.arm(command.armAtUs);
        injected.push_back(command.press);
      } else if (command.kind == Command::Kind::Act) {
        probe.arm(command.armAtUs);
        const char* verb = command.action == ActionVerb::Open         ? "open:"
                           : command.action == ActionVerb::Dictionary ? "dict:"
                           : command.action == ActionVerb::Idle       ? "idle:"
                           : command.action == ActionVerb::Goto       ? "goto:"
                                                                      : "menu:";
        acted.push_back(std::string(verb) + command.arg);
      }
    }
  }

  void advance(uint64_t us) {
    now += us;
    drain();
  }

  // The call is taken to have begun 1 ms before it completed unless a start is given.
  void display(DisplayCall call, uint32_t mode, uint64_t endUs, uint64_t startUs = 0) {
    probe.onDisplayCall(static_cast<uint8_t>(call), mode, startUs ? startUs : endUs - 1000, endUs);
  }

  std::vector<StepRecord> records() const {
    std::vector<StepRecord> result;
    for (const auto& command : commands) {
      if (command.kind == Command::Kind::Record) result.push_back(command.record);
    }
    return result;
  }

  const Summary* finished() const {
    for (const auto& command : commands) {
      if (command.kind == Command::Kind::Finish) return &command.summary;
    }
    return nullptr;
  }
};

// ---- parsing -------------------------------------------------------------------------------------------------

TEST(NavScriptParse, ReadsEveryField) {
  const Step step = parse(
      "settings.nested guard=Settings lead=Down*2,Confirm@300 key=Back@90 x=3 expect=Home needs=stats quiet=400 "
      "norefresh=1500 timeout=9000 soft");
  EXPECT_STREQ(step.id, "settings.nested");
  EXPECT_EQ(step.idHash, id("settings.nested"));
  EXPECT_FALSE(step.anyGuard);
  EXPECT_EQ(step.guardId, id("Settings"));
  ASSERT_EQ(step.leadCount, 3);
  EXPECT_EQ(step.lead[0].key, Key::Down);
  EXPECT_EQ(step.lead[1].key, Key::Down);
  EXPECT_EQ(step.lead[0].holdMs, 60);
  EXPECT_EQ(step.lead[2].key, Key::Confirm);
  EXPECT_EQ(step.lead[2].holdMs, 300);
  ASSERT_TRUE(step.hasKey);
  EXPECT_EQ(step.key.key, Key::Back);
  EXPECT_EQ(step.key.holdMs, 90);
  EXPECT_EQ(step.reps, 3);
  ASSERT_TRUE(step.hasExpect);
  EXPECT_EQ(step.expectId, id("Home"));
  ASSERT_TRUE(step.hasNeeds);
  EXPECT_EQ(step.needsId, id("stats"));
  EXPECT_EQ(step.quietMs, 400);
  EXPECT_EQ(step.noRefreshMs, 1500);
  EXPECT_EQ(step.timeoutMs, 9000u);
  EXPECT_EQ(step.flags, FlagSoft);
}

TEST(NavScriptParse, DefaultsAndAnyGuard) {
  const Step step = parse("home.move.0 guard=any key=PageForward");
  EXPECT_TRUE(step.anyGuard);
  EXPECT_EQ(step.reps, 1);
  EXPECT_EQ(step.quietMs, 700);
  EXPECT_EQ(step.noRefreshMs, 2500);
  EXPECT_EQ(step.timeoutMs, 120000u);
  EXPECT_EQ(step.key.key, Key::PageForward);
  EXPECT_FALSE(step.hasExpect);
  EXPECT_FALSE(step.hasNeeds);
}

TEST(NavScriptParse, CommentsAndBlankLinesAreSkippedButCounted) {
  const char* script =
      "# header\n"
      "\n"
      "  \t\n"
      "a.one guard=Home key=Confirm # trailing comment\n"
      "a.two guard=Home key=Down bogus=1\n";
  size_t count = 0;
  const ParseError error = check(script, &count);
  EXPECT_EQ(error.code, ParseCode::UnknownField);
  EXPECT_EQ(error.line, 5u);
  const char* good = "# header\n\na.one guard=Home key=Confirm # trailing\n";
  EXPECT_EQ(check(good, &count).code, ParseCode::Ok);
  EXPECT_EQ(count, 1u);
}

TEST(NavScriptParse, RejectsMalformedLinesWithCodeAndLine) {
  struct Case {
    const char* script;
    ParseCode code;
  };
  const Case cases[] = {
      {"A.upper guard=Home key=Down\n", ParseCode::BadStepId},
      {"a guard=Home key=Sideways\n", ParseCode::UnknownKey},
      {"a guard=Home key=Down@0\n", ParseCode::BadNumber},
      {"a guard=Home key=Down*2\n", ParseCode::BadNumber},
      {"a guard=Home key=Down x=0\n", ParseCode::BadNumber},
      {"a guard=Home key=Down x=256\n", ParseCode::BadNumber},
      {"a guard=Home key=Down quiet=abc\n", ParseCode::BadNumber},
      {"a key=Down\n", ParseCode::MissingGuard},
      {"a guard= key=Down\n", ParseCode::MissingGuard},
      {"a guard=Home\n", ParseCode::NoAction},
      {"a guard=Home external key=Down\n", ParseCode::NoAction},
      {"a guard=Home lead=Down*9,Up*9\n", ParseCode::TooManyLead},
      {"a guard=Home key=Down frobnicate\n", ParseCode::UnknownField},
      {"a guard=Home key=Down timeout=3600001\n", ParseCode::BadNumber},
      {"a guard=Home key=Down needs=averyveryveryverylongfeaturenamethatdoesnotfit\n", ParseCode::TokenTooLong},
      {"# only a comment\n\n", ParseCode::NoSteps},
      {"", ParseCode::NoSteps},
  };
  for (const auto& test : cases) {
    EXPECT_EQ(check(test.script).code, test.code) << test.script;
  }
  EXPECT_EQ(check(nullptr).code, ParseCode::NoSteps);
}

TEST(NavScriptParse, ReadsTheReaderMenuAction) {
  EXPECT_EQ(parse("m guard=Home do=menu:list setup").action, ActionVerb::MenuStyle);
  EXPECT_STREQ(parse("m guard=Home do=menu:toolbar").arg, "toolbar");
  EXPECT_NE(check("m guard=Home do=menu:sidebar\n").code, ParseCode::Ok);
  EXPECT_NE(check("m guard=Home do=menu:listing\n").code, ParseCode::Ok);
}

TEST(NavScriptParse, VariantsForDifferentPrerequisitesMayShareAnId) {
  EXPECT_EQ(check("a guard=Home needs=opds key=Down\na guard=Home needs=noopds key=Up\n").code, ParseCode::Ok);
  EXPECT_EQ(check("a guard=Home needs=opds key=Down\na guard=Home needs=opds key=Up\n").code, ParseCode::DuplicateId);
  EXPECT_EQ(check("a guard=Home key=Down\na guard=Home needs=opds key=Up\n").code, ParseCode::DuplicateId);
  EXPECT_EQ(check("a guard=Home needs=opds key=Down\na guard=Home key=Up\n").code, ParseCode::DuplicateId);
}

TEST(NavScriptParse, ReadsSoftwareActions) {
  const Step open = parse("book.open guard=Home do=open:/navfeat/spinebook.epub expect=EpubReader");
  EXPECT_EQ(open.action, ActionVerb::Open);
  EXPECT_STREQ(open.arg, "/navfeat/spinebook.epub");
  EXPECT_FALSE(open.hasKey);
  const Step dict = parse("dict.set guard=any do=dict:navigation setup");
  EXPECT_EQ(dict.action, ActionVerb::Dictionary);
  EXPECT_STREQ(dict.arg, "navigation");
  EXPECT_NE(dict.flags & FlagSetup, 0);
  const Step withLead = parse("x guard=Home lead=Down do=open:/a.epub");
  EXPECT_EQ(withLead.leadCount, 1u);
}

TEST(NavScriptParse, ReadsTheIdleActionAndTheBlindFlag) {
  const Step idle = parse("txt.index.complete guard=TxtReader do=idle:index norefresh=30000 blind");
  EXPECT_EQ(idle.action, ActionVerb::Idle);
  EXPECT_STREQ(idle.arg, "index");
  EXPECT_NE(idle.flags & FlagBlind, 0);
  EXPECT_EQ(parse("a guard=Home key=Down").flags & FlagBlind, 0);
  EXPECT_NE(check("a guard=Home do=idle:\n").code, ParseCode::Ok);
  EXPECT_NE(check("a guard=Home do=idle:a;b\n").code, ParseCode::Ok);
  EXPECT_NE(check("a guard=Home blind=1 key=Down\n").code, ParseCode::Ok);
}

TEST(NavScriptParse, AnIdThatOnlyStartsLikeAnEarlierOneIsNotADuplicate) {
  EXPECT_EQ(check("wifi.ui guard=Home key=Down\nwifi.ui.open guard=Home key=Down\nwifi.u guard=Home key=Up\n").code,
            ParseCode::Ok);
  EXPECT_EQ(check("wifi.ui guard=Home key=Down\n  wifi.ui guard=Home key=Up\n").code, ParseCode::DuplicateId);
}

TEST(NavScriptParse, GotoOpensOnlyTheLibraryOrHome) {
  const Step library = parse("lib.rt.open guard=Home do=goto:library expect=Library");
  EXPECT_EQ(library.action, ActionVerb::Goto);
  EXPECT_STREQ(library.arg, "library");
  const Step home = parse("prep.home guard=BenchmarkSettings do=goto:home expect=Home setup");
  EXPECT_EQ(home.action, ActionVerb::Goto);
  EXPECT_STREQ(home.arg, "home");
  EXPECT_TRUE(home.flags & FlagSetup);
  EXPECT_EQ(home.guardId, id("BenchmarkSettings"));
  EXPECT_EQ(home.expectId, id("Home"));
  EXPECT_FALSE(home.hasKey);
  EXPECT_NE(check("a guard=Home do=goto:settings\n").code, ParseCode::Ok);
  EXPECT_NE(check("a guard=Home do=goto:librarys\n").code, ParseCode::Ok);
  EXPECT_NE(check("a guard=Home do=goto:homes\n").code, ParseCode::Ok);
  EXPECT_NE(check("a guard=Home do=goto:hom\n").code, ParseCode::Ok);
  EXPECT_NE(check("a guard=Home do=goto:Home\n").code, ParseCode::Ok);  // the arguments are lower case
  EXPECT_NE(check("a guard=Home do=goto:\n").code, ParseCode::Ok);
  EXPECT_NE(check("a guard=Home key=Down do=goto:library\n").code, ParseCode::Ok);
  EXPECT_NE(check("a guard=Home key=Down do=goto:home\n").code, ParseCode::Ok);
  EXPECT_NE(check("a guard=Home external do=goto:home\n").code, ParseCode::Ok);
}

TEST(NavScriptParse, RejectsMalformedSoftwareActions) {
  const char* bad[] = {
      "a guard=Home do=open:\n",                      // no path
      "a guard=Home do=open:relative.epub\n",         // not absolute
      "a guard=Home do=open:/a/../b.epub\n",          // parent reference
      "a guard=Home do=open:/a b.epub\n",             // space splits the token: a stray field
      "a guard=Home do=open:/a;b.epub\n",             // character outside the path alphabet
      "a guard=Home do=dict:a/b\n",                   // a folder name, not a path
      "a guard=Home do=dict:\n",
      "a guard=Home do=wipe:everything\n",            // unknown verb
      "a guard=Home do=open\n",                       // no argument
      "a guard=Home key=Down do=open:/a.epub\n",      // a step has a key or an action
      "a guard=Home external do=open:/a.epub\n",
      "a guard=Home do=open:/a.epub do=open:/b.epub\n",
  };
  for (const char* script : bad) EXPECT_NE(check(script).code, ParseCode::Ok) << script;
  std::string longPath = "a guard=Home do=open:/";
  longPath.append(80, 'a');
  longPath += "\n";
  EXPECT_EQ(check(longPath.c_str()).code, ParseCode::TokenTooLong);
}

TEST(NavSequencer, ActionStepArmsTheProbeThenSettlesLikeAKey) {
  Rig rig;
  ASSERT_TRUE(rig.begin("book.open guard=Home do=open:/navfeat/probebook.epub expect=EpubReader quiet=100 norefresh=500 timeout=5000\n"));
  rig.drain();
  ASSERT_EQ(rig.acted.size(), 1u);
  EXPECT_EQ(rig.acted[0], "open:/navfeat/probebook.epub");
  EXPECT_TRUE(rig.injected.empty());  // no key is pressed
  const uint64_t injectAt = rig.now;
  rig.now += 4000;
  rig.activity = id("EpubReader");
  rig.display(DisplayCall::Buffer, 1, rig.now);
  rig.advance(300000);
  ASSERT_NE(rig.finished(), nullptr);
  const auto records = rig.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].status, StepStatus::Ok);
  EXPECT_EQ(records[0].injectUs, injectAt);
  EXPECT_GT(records[0].settledUs, injectAt);
}

TEST(NavSequencer, GotoHomeIsOneActionFromTheBenchmarkScreenAndPressesNoKey) {
  Rig rig;
  rig.activity = id("BenchmarkSettings");
  ASSERT_TRUE(rig.begin("prep.home guard=BenchmarkSettings do=goto:home expect=Home setup quiet=100 norefresh=500 timeout=5000\n"
                        "next guard=Home key=Down quiet=100 norefresh=500\n"));
  rig.drain();
  ASSERT_EQ(rig.acted.size(), 1u);
  EXPECT_EQ(rig.acted[0], "goto:home");
  EXPECT_TRUE(rig.injected.empty());  // Settings focus and tab are never touched: one Replace, no Back presses
  rig.now += 4000;
  rig.activity = id("Home");
  rig.display(DisplayCall::Buffer, 1, rig.now);
  rig.advance(300000);
  EXPECT_EQ(rig.acted.size(), 1u);
  EXPECT_EQ(rig.injected.size(), 1u);  // only now does the next step press its key
}

TEST(NavSequencer, GotoHomeFromAnyOtherScreenDivergesWithoutActing) {
  for (const char* screen : {"Settings", "Home", "Library"}) {
    Rig rig;
    rig.activity = id(screen);
    ASSERT_TRUE(rig.begin("prep.home guard=BenchmarkSettings do=goto:home expect=Home setup\n"));
    rig.drain();
    EXPECT_TRUE(rig.acted.empty()) << screen;
    ASSERT_NE(rig.finished(), nullptr) << screen;
    EXPECT_EQ(rig.finished()->outcome, Outcome::Diverged) << screen;
  }
}

TEST(NavSequencer, BlindStepSettlesAsUnverifiedNeverOk) {
  Rig rig;
  ASSERT_TRUE(rig.begin(
      "panel.open guard=EpubReader key=Confirm blind quiet=100 norefresh=500\n"
      "turn guard=EpubReader key=Down quiet=100 norefresh=500\n"));
  rig.activity = id("EpubReader");
  rig.drain();
  rig.now += 4000;
  rig.display(DisplayCall::Refresh, 1, rig.now);
  rig.advance(300000);  // blind step settled with a refresh
  rig.now += 4000;
  rig.display(DisplayCall::Refresh, 1, rig.now);
  rig.advance(300000);
  const Summary* summary = rig.finished();
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->unverified, 1u);
  EXPECT_EQ(summary->ok, 1u);
  EXPECT_EQ(summary->failed, 0u);
  const auto records = rig.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].status, StepStatus::Unverified);
  EXPECT_EQ(records[1].status, StepStatus::Ok);
}

TEST(NavSequencer, BlindStepWithoutARefreshIsUnverifiedNotNoRefresh) {
  Rig rig;
  ASSERT_TRUE(rig.begin("wait guard=TxtReader do=idle:index norefresh=400 blind\n"));
  rig.activity = id("TxtReader");
  rig.drain();
  ASSERT_EQ(rig.acted.size(), 1u);
  EXPECT_EQ(rig.acted[0], "idle:index");
  EXPECT_TRUE(rig.injected.empty());
  rig.advance(500000);
  const Summary* summary = rig.finished();
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->unverified, 1u);
  EXPECT_EQ(summary->noRefresh, 0u);
  EXPECT_EQ(summary->ok, 0u);
}

TEST(NavSequencer, BlindStepThatNeverSettlesStillFailsTheRun) {
  Rig rig;
  ASSERT_TRUE(rig.begin("panel guard=EpubReader key=Confirm blind expect=ReadingStats timeout=1000\n"));
  rig.activity = id("EpubReader");
  rig.drain();
  rig.advance(1500000);
  const Summary* summary = rig.finished();
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->outcome, Outcome::Diverged);
  EXPECT_EQ(summary->unverified, 0u);
}

TEST(NavScriptParse, ReadsTheSyncFlagOnlyOnASingleKeyPress) {
  const Step step = parse("sync.now guard=ReaderSyncSettings lead=Up key=Confirm quiet=2000 sync");
  EXPECT_NE(step.flags & FlagSync, 0);
  EXPECT_EQ(step.flags & FlagBlind, 0);
  EXPECT_EQ(parse("a guard=Home key=Down").flags & FlagSync, 0);
  EXPECT_EQ(check("a guard=Home key=Confirm sync\n").code, ParseCode::Ok);
  EXPECT_EQ(check("a guard=Home lead=Up sync\n").code, ParseCode::BadAction);        // no measured key
  EXPECT_EQ(check("a guard=Home key=Confirm x=2 sync\n").code, ParseCode::BadAction);  // never repeated
  EXPECT_EQ(check("a guard=Home do=idle:index sync\n").code, ParseCode::BadAction);  // never a software action
  EXPECT_NE(check("a guard=Home key=Confirm sync=1\n").code, ParseCode::Ok);
}

TEST(NavSequencer, SyncStepDoesNotSettleWhileTheRealAttemptIsPending) {
  Rig rig;
  ASSERT_TRUE(rig.begin("sync.now guard=ReaderSyncSettings key=Confirm sync quiet=100 norefresh=500 timeout=60000\n"));
  rig.activity = id("ReaderSyncSettings");
  rig.drain();
  ASSERT_EQ(rig.injected.size(), 1u);
  ASSERT_FALSE(rig.commands.empty());
  EXPECT_EQ(rig.commands.back().kind, Command::Kind::Inject);
  EXPECT_NE(rig.commands.back().stepFlags & FlagSync, 0);
  rig.syncPending = true;  // the device saw the press permitted and no terminal status yet
  rig.now += 4000;
  rig.display(DisplayCall::Refresh, 1, rig.now);
  rig.advance(5000000);  // far past quiet and norefresh
  EXPECT_TRUE(rig.records().empty());
  EXPECT_EQ(rig.finished(), nullptr);
  rig.syncPending = false;  // terminal status reached
  rig.advance(300000);
  const Summary* summary = rig.finished();
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->outcome, Outcome::Complete);
  EXPECT_EQ(summary->ok, 1u);
  EXPECT_EQ(summary->unverified, 0u);
  const auto records = rig.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].status, StepStatus::Ok);
}

TEST(NavSequencer, SyncStepWhoseAttemptNeverEndsTimesOutAndFailsTheRun) {
  Rig rig;
  ASSERT_TRUE(rig.begin("sync.now guard=ReaderSyncSettings key=Confirm sync quiet=100 norefresh=500 timeout=1000\n"));
  rig.activity = id("ReaderSyncSettings");
  rig.drain();
  rig.syncPending = true;
  rig.advance(1500000);
  const Summary* summary = rig.finished();
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->outcome, Outcome::Timeout);
  EXPECT_EQ(summary->failed, 1u);
  const auto records = rig.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].status, StepStatus::Timeout);
  EXPECT_NE(rig.commands[rig.commands.size() - 2].stepFlags & FlagSync, 0);  // the record names the sync step
}

TEST(NavSequencer, ActionStepNeverReachingItsScreenDivergesTheRun) {
  Rig rig;
  ASSERT_TRUE(rig.begin(
      "book.open guard=Home do=open:/navfeat/probebook.epub expect=EpubReader timeout=1000\n"
      "next guard=EpubReader key=Down\n"));
  rig.drain();
  rig.advance(1500000);
  const Summary* summary = rig.finished();
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->outcome, Outcome::Diverged);
  EXPECT_EQ(rig.injected.size(), 0u);
}

TEST(NavSequencer, ActionStepWithUnmetGuardOrFeatureNeverActs) {
  Rig rig;
  ASSERT_TRUE(rig.begin(
      "a guard=Home needs=bigepub do=open:/navfeat/spinebook.epub\n"
      "b guard=Settings do=open:/navfeat/probebook.epub\n"));
  rig.drain();
  EXPECT_TRUE(rig.acted.empty());
  ASSERT_NE(rig.finished(), nullptr);
  EXPECT_EQ(rig.finished()->outcome, Outcome::Diverged);
  EXPECT_EQ(rig.finished()->skipped, 1u);
}

TEST(NavScriptParse, RejectsDuplicateStepIdsAtTheSecondOccurrence) {
  const ParseError error = check(
      "a.one guard=Home key=Down\n"
      "# gap\n"
      "a.two guard=Home key=Down\n"
      "a.one guard=Home key=Up\n");
  EXPECT_EQ(error.code, ParseCode::DuplicateId);
  EXPECT_EQ(error.line, 4u);
}

TEST(NavScriptParse, ExternalStepsNeedNoKeyAndStillRequireAGuard) {
  EXPECT_EQ(check("sleep.home guard=Home external\n").code, ParseCode::Ok);
  EXPECT_EQ(check("sleep.home external\n").code, ParseCode::MissingGuard);
}

TEST(NavScriptParse, EnforcesTheStepLimit) {
  std::string script;
  for (size_t i = 0; i <= MAX_STEPS; ++i) script += "s" + std::to_string(i) + " guard=any key=Down\n";
  const ParseError error = check(script.c_str());
  EXPECT_EQ(error.code, ParseCode::TooManySteps);
  EXPECT_EQ(error.line, MAX_STEPS + 1);
}

// ---- refresh probe -------------------------------------------------------------------------------------------

void call(RefreshProbe& probe, DisplayCall kind, uint32_t mode, uint64_t startUs, uint64_t endUs) {
  probe.onDisplayCall(static_cast<uint8_t>(kind), mode, startUs, endUs);
}

TEST(NavRefreshProbe, CountsBufferRefreshAndGrayCallsOnly) {
  RefreshProbe probe;
  probe.arm(100);
  call(probe, DisplayCall::Precondition, 0, 120, 150);
  call(probe, DisplayCall::Sleep, 0, 155, 160);
  EXPECT_EQ(probe.snapshot().count, 0u);
  call(probe, DisplayCall::Buffer, 2, 180, 200);
  call(probe, DisplayCall::Gray, 0, 500, 900);
  const auto snapshot = probe.snapshot();
  EXPECT_EQ(snapshot.count, 2u);
  EXPECT_EQ(snapshot.firstStartUs, 180u);  // when the call began, not when it completed
  EXPECT_EQ(snapshot.firstCall, RefreshCall::Buffer);
  EXPECT_EQ(snapshot.firstMode, 2);
  EXPECT_EQ(snapshot.lastEndUs, 900u);
  EXPECT_EQ(snapshot.lastCall, RefreshCall::Gray);
}

TEST(NavRefreshProbe, RegionClearCountsAsAVisiblePartialUpdate) {
  RefreshProbe probe;
  probe.arm(100);
  call(probe, DisplayCall::RegionClear, 3, 120, 180);
  const auto snapshot = probe.snapshot();
  EXPECT_EQ(snapshot.count, 1u);
  EXPECT_EQ(snapshot.firstStartUs, 120u);
  EXPECT_EQ(snapshot.firstCall, RefreshCall::RegionClear);
  EXPECT_EQ(snapshot.firstMode, 3);
  EXPECT_EQ(snapshot.lastEndUs, 180u);
  EXPECT_EQ(snapshot.lastCall, RefreshCall::RegionClear);
}

TEST(NavRefreshProbe, AsyncRefreshOnlyCountsWhenItsWaitCompletesAfterASubmit) {
  RefreshProbe probe;
  probe.arm(100);
  call(probe, DisplayCall::AsyncWait, 0, 120, 150);  // nothing in flight: not a refresh
  EXPECT_EQ(probe.snapshot().count, 0u);
  call(probe, DisplayCall::AsyncSubmit, 1, 180, 200);  // submission alone is not visible
  EXPECT_EQ(probe.snapshot().count, 0u);
  call(probe, DisplayCall::AsyncWait, 0, 250, 700);
  const auto snapshot = probe.snapshot();
  EXPECT_EQ(snapshot.count, 1u);
  EXPECT_EQ(snapshot.firstCall, RefreshCall::AsyncWait);
  EXPECT_EQ(snapshot.firstMode, 1);        // the submitted mode, not the wait's
  EXPECT_EQ(snapshot.firstStartUs, 180u);  // the submission began the refresh
  EXPECT_EQ(snapshot.lastEndUs, 700u);
  call(probe, DisplayCall::AsyncWait, 0, 710, 800);  // already consumed
  EXPECT_EQ(probe.snapshot().count, 1u);
}

TEST(NavRefreshProbe, ArmingForgetsEarlierSubmissionsAndCallsThatBeganBeforeThePress) {
  RefreshProbe probe;
  probe.arm(0);
  call(probe, DisplayCall::AsyncSubmit, 2, 40, 50);
  call(probe, DisplayCall::Buffer, 0, 55, 60);
  ASSERT_EQ(probe.snapshot().count, 1u);
  probe.arm(1000);
  EXPECT_EQ(probe.snapshot().count, 0u);
  call(probe, DisplayCall::AsyncWait, 0, 1100, 1200);  // pre-arm submit is not ours
  call(probe, DisplayCall::Buffer, 0, 990, 1500);      // began before the press: not caused by it
  EXPECT_EQ(probe.snapshot().count, 0u);
  call(probe, DisplayCall::AsyncSubmit, 1, 900, 1010);  // submitted before the press, completes after it
  call(probe, DisplayCall::AsyncWait, 0, 1020, 1600);
  EXPECT_EQ(probe.snapshot().count, 0u);
  call(probe, DisplayCall::Refresh, 1, 1000, 1400);
  EXPECT_EQ(probe.snapshot().count, 1u);
  EXPECT_EQ(probe.armedAtUs(), 1000u);
}

// ---- sequencing ----------------------------------------------------------------------------------------------

TEST(NavSequencer, InvalidScriptNeverInjectsAndReportsBadScript) {
  Rig rig;
  EXPECT_FALSE(rig.begin("a guard=Home key=Down\nb guard=Home key=Nope\n"));
  EXPECT_TRUE(rig.sequencer.finished());
  EXPECT_EQ(rig.sequencer.summary().outcome, Outcome::BadScript);
  EXPECT_EQ(rig.sequencer.summary().error.code, ParseCode::UnknownKey);
  EXPECT_EQ(rig.sequencer.summary().error.line, 2u);
  rig.drain();
  EXPECT_TRUE(rig.commands.empty());
  EXPECT_TRUE(rig.injected.empty());
  const Event event = encodeResult(rig.sequencer.summary(), 77);
  EXPECT_EQ(event.data[0], static_cast<uint32_t>(Outcome::BadScript));
  EXPECT_EQ(event.data[4], 2u);
  EXPECT_EQ(event.data[5], static_cast<uint32_t>(ParseCode::UnknownKey));
}

TEST(NavSequencer, MeasuresInjectionToFirstRefreshAndToSettled) {
  Rig rig;
  ASSERT_TRUE(rig.begin("home.open guard=Home key=Confirm quiet=500 norefresh=2000\n"));
  rig.drain();
  ASSERT_EQ(rig.injected.size(), 1u);
  EXPECT_EQ(rig.injected[0].key, Key::Confirm);
  ASSERT_EQ(rig.commands.size(), 1u);
  EXPECT_EQ(rig.commands[0].armAtUs, 1000000u);

  rig.heapFree = 95000;
  rig.display(DisplayCall::Buffer, 2, 1200000);
  rig.advance(300000);  // 1.3 s: inside the quiet window of the first refresh
  EXPECT_TRUE(rig.records().empty());
  rig.display(DisplayCall::Gray, 0, 1900000);  // a follow-up refresh restarts the quiet window
  rig.advance(500000);                          // 1.8 s
  rig.advance(500000);                          // 2.3 s, but quiet needs 1.9 + 0.5
  EXPECT_TRUE(rig.records().empty());
  rig.heapFree = 99000;
  rig.advance(150000);  // 2.45 s
  const auto records = rig.records();
  ASSERT_EQ(records.size(), 1u);
  const StepRecord& record = records[0];
  EXPECT_EQ(record.stepIndex, 1u);
  EXPECT_EQ(record.rep, 0u);
  EXPECT_EQ(record.idHash, id("home.open"));
  EXPECT_EQ(record.status, StepStatus::Ok);
  EXPECT_EQ(record.injectUs, 1000000u);
  EXPECT_EQ(record.firstSubmitUs, 1199000u);
  EXPECT_EQ(record.settledUs, 1900000u);
  EXPECT_EQ(record.refreshCount, 2u);
  EXPECT_EQ(record.firstCall, RefreshCall::Buffer);
  EXPECT_EQ(record.firstMode, 2);
  EXPECT_EQ(record.lastCall, RefreshCall::Gray);
  EXPECT_EQ(record.heapMinEver, 90000u);
  EXPECT_EQ(record.heapMinSampled, 95000u);  // lowest free seen while the step ran, not the latest sample
  ASSERT_NE(rig.finished(), nullptr);
  EXPECT_EQ(rig.finished()->outcome, Outcome::Complete);
  EXPECT_EQ(rig.finished()->measured, 1u);
  EXPECT_EQ(rig.finished()->ok, 1u);
}

TEST(NavSequencer, RefreshBeforeInjectionIsNotAttributed) {
  Rig rig;
  ASSERT_TRUE(rig.begin("a guard=Home key=Down quiet=300 norefresh=1000\n"));
  rig.display(DisplayCall::Buffer, 0, 900000);  // before the press: never counts
  rig.drain();
  rig.advance(1100000);
  const auto records = rig.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].status, StepStatus::NoRefresh);
  EXPECT_EQ(records[0].refreshCount, 0u);
  EXPECT_EQ(records[0].firstSubmitUs, 0u);
}

TEST(NavSequencer, StepWithoutRefreshSettlesOnlyAfterTheNoRefreshWindow) {
  Rig rig;
  ASSERT_TRUE(rig.begin("a guard=Home key=Down quiet=300 norefresh=1000\n"));
  rig.drain();
  rig.advance(999999);
  EXPECT_TRUE(rig.records().empty());
  rig.advance(1);
  const auto records = rig.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].status, StepStatus::NoRefresh);
  EXPECT_EQ(records[0].settledUs, 2000000u);
  EXPECT_EQ(rig.finished()->noRefresh, 1u);
  EXPECT_EQ(rig.finished()->ok, 0u);
}

TEST(NavSequencer, BusyRenderBlocksSettling) {
  Rig rig;
  ASSERT_TRUE(rig.begin("a guard=Home key=Confirm quiet=100 norefresh=500\n"));
  rig.drain();
  rig.display(DisplayCall::Refresh, 1, 1050000);
  rig.busy = true;
  rig.advance(5000000);
  EXPECT_TRUE(rig.records().empty());
  rig.busy = false;
  rig.advance(1);
  ASSERT_EQ(rig.records().size(), 1u);
  EXPECT_EQ(rig.records()[0].status, StepStatus::Ok);
}

TEST(NavSequencer, RepeatsRunOneAtATimeWithTheirOwnRecords) {
  Rig rig;
  ASSERT_TRUE(rig.begin("home.move guard=Home key=Down@80 x=3 quiet=100 norefresh=200\n"));
  rig.drain();
  for (int rep = 0; rep < 3; ++rep) {
    ASSERT_EQ(rig.injected.size(), static_cast<size_t>(rep) + 1) << rep;
    rig.display(DisplayCall::Buffer, 2, rig.now + 50000);
    rig.advance(400000);
  }
  EXPECT_EQ(rig.injected.size(), 3u);
  for (const auto& press : rig.injected) {
    EXPECT_EQ(press.key, Key::Down);
    EXPECT_EQ(press.holdMs, 80);
  }
  const auto records = rig.records();
  ASSERT_EQ(records.size(), 3u);
  for (uint32_t rep = 0; rep < 3; ++rep) {
    EXPECT_EQ(records[rep].rep, rep);
    EXPECT_EQ(records[rep].stepIndex, 1u);
    EXPECT_EQ(records[rep].status, StepStatus::Ok);
  }
  EXPECT_LT(records[0].settledUs, records[1].injectUs);  // the next press waits for the previous to settle
  EXPECT_EQ(rig.finished()->measured, 3u);
}

TEST(NavSequencer, LeadPressesSettleOneByOneAndAreNotRecorded) {
  Rig rig;
  ASSERT_TRUE(rig.begin("jump guard=Home lead=Down*2 key=Confirm quiet=100 norefresh=300\n"));
  rig.drain();
  ASSERT_EQ(rig.injected.size(), 1u);
  EXPECT_EQ(rig.injected[0].key, Key::Down);
  rig.advance(300000);
  ASSERT_EQ(rig.injected.size(), 2u);
  rig.advance(300000);
  ASSERT_EQ(rig.injected.size(), 3u);
  EXPECT_EQ(rig.injected[2].key, Key::Confirm);
  EXPECT_TRUE(rig.records().empty());
  rig.advance(300000);
  ASSERT_EQ(rig.records().size(), 1u);  // only the measured press
  EXPECT_EQ(rig.finished()->measured, 1u);
}

TEST(NavSequencer, SetupStepsInjectButWriteNoRows) {
  Rig rig;
  ASSERT_TRUE(rig.begin("prep guard=Home key=Down setup norefresh=200 quiet=100\nreal guard=Home key=Up norefresh=200 quiet=100\n"));
  rig.drain();
  rig.advance(300000);
  rig.advance(300000);
  EXPECT_EQ(rig.injected.size(), 2u);
  const auto records = rig.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].idHash, id("real"));
  EXPECT_EQ(records[0].stepIndex, 2u);  // indexes still count the setup line
}

TEST(NavSequencer, OptionalStepWithUnmetGuardIsSkippedWithoutInjectingAndLaterStepsStillRun) {
  Rig rig;
  ASSERT_TRUE(rig.begin(
      "reader.menu guard=EpubReader key=Confirm optional\n"
      "home.move guard=Home key=Down quiet=100 norefresh=200\n"));
  rig.drain();
  rig.advance(300000);
  const auto records = rig.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].status, StepStatus::Unsupported);
  EXPECT_EQ(records[0].refreshCount, 0u);
  EXPECT_EQ(records[1].status, StepStatus::NoRefresh);
  ASSERT_EQ(rig.injected.size(), 1u);
  EXPECT_EQ(rig.injected[0].key, Key::Down);  // the skipped step pressed nothing
  EXPECT_EQ(rig.finished()->skipped, 1u);
  EXPECT_EQ(rig.finished()->outcome, Outcome::Complete);
}

TEST(NavSequencer, UnmetGuardOnASupportedStepFailsTheRunInsteadOfSkipping) {
  Rig rig;
  ASSERT_TRUE(rig.begin(
      "reader.menu guard=EpubReader key=Confirm\n"
      "home.move guard=Home key=Down\n"));
  rig.drain();
  ASSERT_EQ(rig.records().size(), 1u);
  EXPECT_EQ(rig.records()[0].status, StepStatus::Diverged);
  EXPECT_EQ(rig.records()[0].refreshCount, 0u);
  ASSERT_NE(rig.finished(), nullptr);
  EXPECT_EQ(rig.finished()->outcome, Outcome::Diverged);
  EXPECT_EQ(rig.finished()->failed, 1u);
  EXPECT_EQ(rig.finished()->skipped, 0u);
  EXPECT_TRUE(rig.injected.empty());  // nothing pressed blind, later steps never ran
}

TEST(NavSequencer, AnyGuardNeverSkips) {
  Rig rig;
  rig.activity = id("SomethingElse");
  ASSERT_TRUE(rig.begin("a guard=any key=Down norefresh=100 quiet=100\n"));
  rig.drain();
  EXPECT_EQ(rig.injected.size(), 1u);
}

TEST(NavSequencer, MissingFeatureSkipsAndAvailableFeatureRuns) {
  const char* script = "dict.lookup guard=Home needs=dictionary key=Confirm norefresh=100 quiet=100\n";
  Rig missing;
  ASSERT_TRUE(missing.begin(script));
  missing.drain();
  ASSERT_EQ(missing.records().size(), 1u);
  EXPECT_EQ(missing.records()[0].status, StepStatus::Skipped);
  EXPECT_TRUE(missing.injected.empty());

  Rig present;
  present.features = {"dictionary"};
  ASSERT_TRUE(present.begin(script));
  present.drain();
  EXPECT_EQ(present.injected.size(), 1u);
  EXPECT_TRUE(present.records().empty());
}

TEST(NavSequencer, ExternalStepsAreRecordedAsExternalAndNeverInjected) {
  Rig rig;
  ASSERT_TRUE(rig.begin("sleep.home guard=Home external\n"));
  rig.drain();
  ASSERT_EQ(rig.records().size(), 1u);
  EXPECT_EQ(rig.records()[0].status, StepStatus::External);
  EXPECT_TRUE(rig.injected.empty());
  EXPECT_EQ(rig.finished()->skipped, 0u);
  EXPECT_EQ(rig.finished()->external, 1u);
  EXPECT_EQ(rig.finished()->outcome, Outcome::Complete);
}

TEST(NavSequencer, ExpectedActivityMustBeReachedBeforeSettling) {
  Rig rig;
  ASSERT_TRUE(rig.begin("open guard=Home key=Confirm expect=EpubReader quiet=100 norefresh=200 timeout=5000\n"));
  rig.drain();
  rig.display(DisplayCall::Buffer, 2, 1100000);
  rig.advance(1000000);  // quiet long since, but the activity has not changed
  EXPECT_TRUE(rig.records().empty());
  rig.activity = id("EpubReader");
  rig.advance(1);
  ASSERT_EQ(rig.records().size(), 1u);
  EXPECT_EQ(rig.records()[0].status, StepStatus::Ok);
}

TEST(NavSequencer, WrongActivityAtTimeoutDivergesAndEndsTheRun) {
  Rig rig;
  ASSERT_TRUE(rig.begin(
      "open guard=Home key=Confirm expect=EpubReader timeout=1000\n"
      "next guard=Home key=Down\n"));
  rig.drain();
  rig.advance(1000000);
  const auto records = rig.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].status, StepStatus::Diverged);
  ASSERT_NE(rig.finished(), nullptr);
  EXPECT_EQ(rig.finished()->outcome, Outcome::Diverged);
  EXPECT_EQ(rig.finished()->failed, 1u);
  EXPECT_EQ(rig.injected.size(), 1u);  // the next step never ran blind
  EXPECT_TRUE(rig.sequencer.finished());
}

TEST(NavSequencer, SoftExpectationRecordsDivergenceAndContinues) {
  Rig rig;
  ASSERT_TRUE(rig.begin(
      "open guard=Home key=Confirm expect=EpubReader timeout=1000 soft\n"
      "next guard=Home key=Down norefresh=100 quiet=100\n"));
  rig.drain();
  rig.advance(1000000);
  ASSERT_EQ(rig.records().size(), 1u);
  EXPECT_EQ(rig.records()[0].status, StepStatus::Diverged);
  EXPECT_EQ(rig.finished(), nullptr);
  EXPECT_EQ(rig.injected.size(), 2u);
}

TEST(NavSequencer, NeverSettlingTimesOutAndEndsTheRun) {
  Rig rig;
  ASSERT_TRUE(rig.begin("stuck guard=Home key=Confirm timeout=2000\n"));
  rig.drain();
  rig.busy = true;
  rig.advance(1999999);
  EXPECT_TRUE(rig.records().empty());
  rig.advance(1);
  ASSERT_EQ(rig.records().size(), 1u);
  EXPECT_EQ(rig.records()[0].status, StepStatus::Timeout);
  ASSERT_NE(rig.finished(), nullptr);
  EXPECT_EQ(rig.finished()->outcome, Outcome::Timeout);
  EXPECT_EQ(rig.finished()->failed, 1u);
  EXPECT_EQ(rig.finished()->elapsedMs, 2000u);
}

TEST(NavSequencer, LeadPressTimeoutIsRecordedAndFatal) {
  Rig rig;
  ASSERT_TRUE(rig.begin("a guard=Home lead=Down key=Confirm timeout=1000\n"));
  rig.drain();
  rig.busy = true;
  rig.advance(1000000);
  ASSERT_EQ(rig.records().size(), 1u);
  EXPECT_EQ(rig.records()[0].status, StepStatus::Timeout);
  EXPECT_EQ(rig.finished()->outcome, Outcome::Timeout);
  EXPECT_EQ(rig.injected.size(), 1u);  // the measured key was never pressed
}

TEST(NavSequencer, CancelFinishesWithoutFurtherInjection) {
  Rig rig;
  ASSERT_TRUE(rig.begin("a guard=Home key=Down x=5\n"));
  rig.drain();
  rig.sequencer.cancel();
  rig.advance(10);
  ASSERT_NE(rig.finished(), nullptr);
  EXPECT_EQ(rig.finished()->outcome, Outcome::Cancelled);
  EXPECT_EQ(rig.injected.size(), 1u);
  rig.advance(100000000);
  EXPECT_EQ(rig.injected.size(), 1u);
  EXPECT_EQ(rig.commands.back().kind, Command::Kind::Finish);
}

TEST(NavSequencer, AbortCarriesItsOutcomeAndStopsInjecting) {
  for (const Outcome outcome : {Outcome::Unmappable, Outcome::Interrupted, Outcome::Recorder}) {
    Rig rig;
    ASSERT_TRUE(rig.begin("a guard=Home key=Down x=5\n"));
    rig.drain();
    rig.sequencer.abort(outcome);
    rig.advance(10);
    ASSERT_NE(rig.finished(), nullptr);
    EXPECT_EQ(rig.finished()->outcome, outcome);
    EXPECT_EQ(rig.injected.size(), 1u);
  }
}

TEST(NavSequencer, SummaryCountsEveryKindOfStep) {
  Rig rig;
  rig.features = {};
  ASSERT_TRUE(rig.begin(
      "a guard=Home key=Down quiet=100 norefresh=200\n"
      "b guard=Home key=Down quiet=100 norefresh=200\n"
      "c guard=Other key=Down optional\n"
      "d guard=Home needs=nothing key=Down\n"
      "e guard=Home external\n"));
  rig.drain();
  rig.display(DisplayCall::Buffer, 0, rig.now + 10000);
  rig.advance(300000);
  rig.advance(300000);
  const Summary* summary = rig.finished();
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->outcome, Outcome::Complete);
  EXPECT_EQ(summary->steps, 5u);
  EXPECT_EQ(summary->measured, 2u);
  EXPECT_EQ(summary->ok, 1u);
  EXPECT_EQ(summary->noRefresh, 1u);
  EXPECT_EQ(summary->skipped, 2u);
  EXPECT_EQ(summary->external, 1u);
  EXPECT_EQ(summary->failed, 0u);
  EXPECT_NE(summary->scriptId, 0u);
}

// ---- record encoding -----------------------------------------------------------------------------------------

TEST(NavEncoding, AppendedEventKindsKeepTheirSlots) {
  EXPECT_EQ(static_cast<uint8_t>(EventType::Battery), 17);
  // rc9.3 refresh-calibration rows already exist in recorded device sessions.
  EXPECT_EQ(static_cast<uint8_t>(EventType::RefreshClear), 18);
  EXPECT_EQ(static_cast<uint8_t>(EventType::ManualRefresh), 19);
  EXPECT_EQ(static_cast<uint8_t>(EventType::NavStep), 20);
  EXPECT_EQ(static_cast<uint8_t>(EventType::NavResult), 21);
  EXPECT_EQ(static_cast<uint8_t>(EventType::NavInput), 22);
  EXPECT_EQ(static_cast<uint8_t>(EventType::NavRows), 23);
  EXPECT_EQ(static_cast<uint8_t>(EventType::NavRestore), 24);
  EXPECT_EQ(static_cast<uint8_t>(EventType::Reserved25), 25);
  EXPECT_EQ(static_cast<uint8_t>(EventType::StatsFlush), 26);
  EXPECT_EQ(static_cast<uint8_t>(NavRowsScreen::Library), 1);
  EXPECT_EQ(static_cast<uint8_t>(NavRowsScreen::Contents), 2);
  EXPECT_EQ(static_cast<uint8_t>(NavRowsScreen::LibraryPrepare), 3);
  // Refresh-call codes are packed two per byte in a nav_step row, so their values are recorded too.
  EXPECT_EQ(static_cast<uint8_t>(RefreshCall::ConfiguredGrayBase), 6);
  EXPECT_EQ(static_cast<uint8_t>(RefreshCall::RegionClear), 7);
}

TEST(NavEncoding, StepRowSerializesEveryField) {
  StepRecord record;
  record.stepIndex = 3;
  record.rep = 1;
  record.idHash = 1234;
  record.status = StepStatus::Ok;
  record.injectUs = 1000000;
  record.settledUs = 1900000;
  record.firstSubmitUs = 1200000;
  record.refreshCount = 2;
  record.firstCall = RefreshCall::Buffer;
  record.firstMode = 2;
  record.lastCall = RefreshCall::Gray;
  record.lastMode = 0;
  record.heapMinEver = 90000;
  record.heapMinSampled = 95000;
  Sink sink;
  Writer writer;
  writer.begin(Sink::write, &sink);
  ASSERT_EQ(writer.event(encodeStep(record)), Writer::Result::Written);
  EXPECT_EQ(sink.output,
            "{\"event\":\"nav_step\",\"seq\":0,\"start_us\":1000000,\"end_us\":1900000,\"duration_us\":900000,"
            "\"data\":[3,1,1234,200000,328193,2,90000,95000]}\n");
}

TEST(NavEncoding, StepRowEncodesStatusAndSaturatesLargeCounts) {
  StepRecord record;
  record.status = StepStatus::Diverged;
  record.injectUs = 500;
  record.settledUs = 400;  // settled can never precede the press
  record.refreshCount = 70000;
  record.firstSubmitUs = 500 + 6000000000ULL;
  Event event = encodeStep(record);
  EXPECT_EQ(event.startUs, 500u);
  EXPECT_EQ(event.endUs, 500u);
  EXPECT_EQ(event.data[3], UINT32_MAX);
  EXPECT_EQ(event.data[5], 0xFFFFu | (static_cast<uint32_t>(StepStatus::Diverged) << 16));
  Sink sink;
  Writer writer;
  writer.begin(Sink::write, &sink);
  EXPECT_EQ(writer.event(event), Writer::Result::Written);  // endUs >= startUs: the writer accepts it
}

TEST(NavEncoding, SkippedStepCarriesNoRefreshEvidence) {
  Rig rig;
  ASSERT_TRUE(rig.begin("x guard=Nowhere key=Down optional\n"));
  rig.drain();
  const Event event = encodeStep(rig.records().at(0));
  EXPECT_EQ(event.data[3], 0u);
  EXPECT_EQ(event.data[4], 0u);
  EXPECT_EQ(event.data[5], static_cast<uint32_t>(StepStatus::Unsupported) << 16);
  EXPECT_EQ(event.startUs, event.endUs);
}

TEST(NavEncoding, ResultRowSerializesSummary) {
  Summary summary;
  summary.outcome = Outcome::Complete;
  summary.steps = 5;
  summary.measured = 4;
  summary.ok = 3;
  summary.noRefresh = 1;
  summary.skipped = 1;
  summary.external = 1;
  summary.failed = 0;
  summary.elapsedMs = 12345;
  summary.scriptId = 99;
  Sink sink;
  Writer writer;
  writer.begin(Sink::write, &sink);
  ASSERT_EQ(writer.event(encodeResult(summary, 5000000), true), Writer::Result::Written);
  EXPECT_EQ(sink.output,
            "{\"event\":\"nav_result\",\"seq\":0,\"start_us\":5000000,\"end_us\":5000000,\"duration_us\":0,"
            "\"data\":[0,5,4,65539,65537,0,12345,99]}\n");
}

TEST(NavEncoding, ResultRowCarriesTheUnverifiedCountNextToFailed) {
  Summary summary;
  summary.steps = 3;
  summary.measured = 3;
  summary.unverified = 2;
  summary.failed = 1;
  const Event event = encodeResult(summary, 1000);
  EXPECT_EQ(event.data[5], 1u | (2u << 16));
  EXPECT_EQ(event.data[3], 0u);  // nothing is counted as ok
}

TEST(NavEncoding, StepRowCarriesTheUnverifiedStatus) {
  StepRecord record;
  record.status = StepStatus::Unverified;
  record.refreshCount = 2;
  EXPECT_EQ(encodeStep(record).data[5], 2u | (static_cast<uint32_t>(StepStatus::Unverified) << 16));
}

TEST(NavEncoding, InputMarkerSerializes) {
  Sink sink;
  Writer writer;
  writer.begin(Sink::write, &sink);
  ASSERT_EQ(writer.event(encodeInput(true, false, false, true, 4242, 777)), Writer::Result::Written);
  EXPECT_EQ(sink.output,
            "{\"event\":\"nav_input\",\"seq\":0,\"start_us\":777,\"end_us\":777,\"duration_us\":0,"
            "\"data\":[1,0,0,1,4242,0,0,0]}\n");
}

TEST(NavEncoding, RowsShareTheRecorderLossAccounting) {
  Buffer buffer;
  StepRecord record;
  for (uint32_t i = 0; i < Buffer::CAPACITY + 6; ++i) {
    record.stepIndex = i;
    record.injectUs = record.settledUs = 10 + i;
    buffer.push(encodeStep(record));
  }
  EXPECT_EQ(buffer.size(), Buffer::CAPACITY);
  EXPECT_EQ(buffer.dropped(), 6u);
  Event event;
  ASSERT_TRUE(buffer.pop(event));
  EXPECT_EQ(event.type, EventType::NavStep);
  EXPECT_EQ(event.data[0], 0u);  // the oldest rows survive; the overflow was dropped, not rotated
  EXPECT_EQ(event.sequence, 1u);
  Sink sink;
  Writer writer;
  writer.begin(Sink::write, &sink);
  EXPECT_EQ(writer.event(event), Writer::Result::Written);
  EXPECT_NE(sink.output.find("\"seq\":1,"), std::string::npos);
}

TEST(NavEncoding, TerminalReserveStillAcceptsTheResultAtTheFileLimit) {
  Sink sink;
  sink.retain = false;
  Writer writer;
  writer.begin(Sink::write, &sink);
  const size_t normalLimit = Writer::MAX_BYTES - Writer::TERMINAL_RESERVE;
  const std::string filler(200, 'x');
  while (writer.bytes() < normalLimit) ASSERT_NE(writer.line(filler.data(), filler.size(), true), Writer::Result::Failed);
  StepRecord record;
  record.injectUs = record.settledUs = 5;
  EXPECT_EQ(writer.event(encodeStep(record)), Writer::Result::Limit);
  Summary summary;
  summary.outcome = Outcome::Cancelled;
  EXPECT_EQ(writer.event(encodeResult(summary, 9), true), Writer::Result::Written);
  EXPECT_LE(writer.bytes(), Writer::MAX_BYTES);
}

}  // namespace

namespace {

// Walks the built-in script line by line with the same parser the device uses.
std::vector<Step> defaultSteps() {
  std::vector<Step> steps;
  const char* cursor = nav::DEFAULT_SCRIPT;
  const char* const end = cursor + std::strlen(cursor);
  while (cursor < end) {
    const char* lineEnd = static_cast<const char*>(std::memchr(cursor, '\n', static_cast<size_t>(end - cursor)));
    if (!lineEnd) lineEnd = end;
    Step step;
    bool blank = false;
    EXPECT_EQ(parseLine(cursor, lineEnd, step, blank), ParseCode::Ok);
    if (!blank) steps.push_back(step);
    cursor = lineEnd + 1;
  }
  return steps;
}

TEST(NavDefaultScript, ValidatesWithUniqueStableIds) {
  size_t count = 0;
  const ParseError error = validate(nav::DEFAULT_SCRIPT, count);
  EXPECT_EQ(error.code, ParseCode::Ok);
  EXPECT_EQ(count, defaultSteps().size());
  EXPECT_GT(count, 20u);
}

TEST(NavDefaultScript, StartsWithOneFocusIndependentGotoHomeFromTheBenchmarkScreen) {
  const auto steps = defaultSteps();
  ASSERT_GE(steps.size(), 2u);
  EXPECT_STREQ(steps[0].id, "prep.home");
  EXPECT_TRUE(steps[0].flags & FlagSetup);
  EXPECT_EQ(steps[0].action, ActionVerb::Goto);
  EXPECT_STREQ(steps[0].arg, "home");
  EXPECT_FALSE(steps[0].hasKey);  // no Back press: the Settings focus and tab the run was started from cannot matter
  EXPECT_EQ(steps[0].guardId, id("BenchmarkSettings"));
  EXPECT_EQ(steps[0].expectId, id("Home"));
  for (size_t i = 1; i < steps.size(); ++i) {
    EXPECT_STRNE(steps[i].id, "prep.settings.focus");
    EXPECT_STRNE(steps[i].id, "prep.settings.tabs");
    EXPECT_NE(steps[i].guardId, id("BenchmarkSettings")) << steps[i].id;
  }
}

TEST(NavDefaultScript, EveryMeasuredStepIsGuardedAndNeverBlind) {
  for (const Step& step : defaultSteps()) {
    if (step.flags & FlagExternal) continue;
    EXPECT_FALSE(step.anyGuard) << step.id;
  }
}

TEST(NavDefaultScript, OpdsAndNoOpdsVariantsComeInPairsAndEveryHomeJumpIsCountedFromAKnownRow) {
  const auto steps = defaultSteps();
  for (const Step& step : steps) {
    if (!step.hasNeeds) continue;
    EXPECT_TRUE(step.needsId == id("opds") || step.needsId == id("noopds")) << step.id;
    const std::string name = step.id;
    const std::string suffix = ".opds";
    const bool opds = name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    EXPECT_EQ(step.needsId == id("opds"), opds) << name;
    const std::string partner = opds ? name.substr(0, name.size() - suffix.size()) : name + suffix;
    bool found = false;
    for (const Step& other : steps) found = found || partner == other.id;
    EXPECT_TRUE(found) << name << " has no partner " << partner;
    // An OPDS row sits between Files and File Transfer: it adds one press to jumps that cross it.
    const Step* base = nullptr;
    const Step* extra = nullptr;
    for (const Step& other : steps) {
      if (name == other.id) (opds ? extra : base) = &other;
      if (partner == other.id) (opds ? base : extra) = &other;
    }
    ASSERT_TRUE(base && extra) << name;
    EXPECT_EQ(extra->leadCount, base->leadCount + 1) << name;
  }
}

TEST(NavDefaultScript, NothingMutatingIsPressedOnARowOfUnknownContent) {
  // Confirm is only used to open a Home row; Library/Files/Settings rows are only moved over and left with Back.
  for (const Step& step : defaultSteps()) {
    if (step.hasKey && step.key.key == Key::Confirm) EXPECT_EQ(step.guardId, id("Home")) << step.id;
  }
}

TEST(NavDefaultScript, SleepWakeStepsAreExternalNotInjected) {
  int external = 0;
  for (const Step& step : defaultSteps()) {
    if (step.flags & FlagExternal) ++external;
  }
  EXPECT_EQ(external, 2);
}

}  // namespace
