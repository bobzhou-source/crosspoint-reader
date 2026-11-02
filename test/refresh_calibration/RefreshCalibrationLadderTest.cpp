#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "RefreshCalibrationLadder.h"

namespace rc = refresh_calibration;

namespace {

using rc::ContentClass;
using rc::Ladder;
using rc::Polarity;
using rc::Rung;
using rc::RungKind;

// A simulated viewer: every level above `tolerated` bothers them.
struct Viewer {
  int8_t tolerated[2][rc::kClassCount];
  uint8_t rate(const Rung& r) const {
    if (r.kind == RungKind::Reference) return 0;
    return r.level > tolerated[static_cast<int>(r.polarity)][static_cast<int>(r.cls)] ? 2 : 1;
  }
};

Viewer uniformViewer(int8_t tolerated) {
  Viewer v{};
  for (auto& p : v.tolerated)
    for (auto& c : p) c = tolerated;
  return v;
}

std::vector<Rung> run(Ladder& ladder, const Viewer& viewer) {
  std::vector<Rung> shown;
  while (!ladder.done()) {
    const Rung r = ladder.next();
    shown.push_back(r);
    EXPECT_TRUE(ladder.rate(r, viewer.rate(r)));
    if (shown.size() > 200) break;
  }
  return shown;
}

rc::RungMeasurement sampleMeasurement() {
  rc::RungMeasurement m;
  m.pixelCount = 384000;
  m.frames = 12;
  m.changedPixels = 912345;
  m.weightedDebt = 1140431;
  m.policyClearAt = 8;
  m.meanInkPixels = 38000;
  m.grayscale = true;
  return m;
}

JsonDocument parseLine(const char* line, size_t length) {
  JsonDocument doc;
  EXPECT_FALSE(deserializeJson(doc, line, length)) << line;
  return doc;
}

}  // namespace

TEST(RefreshCalibrationLadder, EachPolarityOpensWithAReferenceThenWalksTheClassesInOrder) {
  Ladder ladder;
  const auto shown = run(ladder, uniformViewer(3));
  ASSERT_FALSE(shown.empty());
  EXPECT_EQ(shown.front().kind, RungKind::Reference);
  EXPECT_EQ(shown.front().polarity, Polarity::Light);

  std::vector<std::pair<Polarity, int>> order;  // (polarity, class or -1 for reference) as first seen
  for (const auto& r : shown) {
    const int cls = r.kind == RungKind::Reference ? -1 : static_cast<int>(r.cls);
    if (order.empty() || order.back() != std::make_pair(r.polarity, cls)) order.emplace_back(r.polarity, cls);
  }
  const std::vector<std::pair<Polarity, int>> expected = {
      {Polarity::Light, -1}, {Polarity::Light, 0}, {Polarity::Light, 1}, {Polarity::Light, 2},
      {Polarity::Light, 3},  {Polarity::Light, 4}, {Polarity::Night, -1}, {Polarity::Night, 0},
      {Polarity::Night, 1},  {Polarity::Night, 2}, {Polarity::Night, 3},  {Polarity::Night, 4}};
  EXPECT_EQ(order, expected);
  for (size_t i = 0; i < shown.size(); ++i) EXPECT_EQ(shown[i].seq, i) << "seq is the run position";
}

TEST(RefreshCalibrationLadder, StaircaseFindsEveryPossibleBoundaryForEverySeries) {
  // tolerated = -1 (even 2 fast frames bother) .. 7 (48 is fine).
  for (int8_t tolerated = -1; tolerated < rc::kLevels; ++tolerated) {
    Ladder ladder;
    const auto shown = run(ladder, uniformViewer(tolerated));
    ASSERT_TRUE(ladder.done());
    for (uint8_t p = 0; p < 2; ++p) {
      for (uint8_t c = 0; c < rc::kClassCount; ++c) {
        const auto& s = ladder.series(static_cast<Polarity>(p), static_cast<ContentClass>(c));
        EXPECT_EQ(s.lo, tolerated) << "tolerated " << int(tolerated) << " p" << int(p) << " c" << int(c);
        EXPECT_EQ(s.hi, tolerated + 1);
        EXPECT_LE(s.rungs, 4u) << "8 levels need at most 4 rungs";
      }
    }
  }
}

TEST(RefreshCalibrationLadder, WarmStartsKeepAConsistentViewerNearTwoRungsPerSeries) {
  Ladder ladder;
  const Viewer viewer = uniformViewer(4);  // 16 fast frames fine, 24 bothers
  const auto shown = run(ladder, viewer);
  // 2 references + the first series (12 ok, 16 ok, 32 bad, 24 bad = 4) + 9 warm series of 2.
  EXPECT_EQ(shown.size(), 2u + 4u + 9u * 2u);
  EXPECT_EQ(ladder.series(Polarity::Light, ContentClass::DenseLatin).start, rc::kFirstStartLevel);
  EXPECT_EQ(ladder.series(Polarity::Light, ContentClass::DenseCjk).start, 4);
  EXPECT_EQ(ladder.series(Polarity::Night, ContentClass::DenseLatin).start, 4);
}

TEST(RefreshCalibrationLadder, NightSeriesStartFromTheSameClassInDaylight) {
  Viewer viewer = uniformViewer(4);
  viewer.tolerated[0][static_cast<int>(ContentClass::GrayImage)] = 1;  // images: only 4 fast frames
  Ladder ladder;
  run(ladder, viewer);
  EXPECT_EQ(ladder.series(Polarity::Light, ContentClass::GrayImage).lo, 1);
  EXPECT_EQ(ladder.series(Polarity::Light, ContentClass::Menu).start, 1);  // previous light series
  EXPECT_EQ(ladder.series(Polarity::Night, ContentClass::GrayImage).start, 1);
  EXPECT_EQ(ladder.series(Polarity::Night, ContentClass::Menu).start, 4);  // light menu boundary
}

TEST(RefreshCalibrationLadder, WorstCaseRunStaysBounded) {
  // A viewer whose tolerance jumps between extremes defeats every warm start.
  Viewer viewer{};
  for (uint8_t p = 0; p < 2; ++p)
    for (uint8_t c = 0; c < rc::kClassCount; ++c) viewer.tolerated[p][c] = (c + p) % 2 ? 7 : -1;
  Ladder ladder;
  const auto shown = run(ladder, viewer);
  EXPECT_TRUE(ladder.done());
  EXPECT_LE(shown.size(), 2u + 10u * 4u);
}

TEST(RefreshCalibrationLadder, NonMonotoneRatingsStillTerminate) {
  Ladder ladder;
  ladder.rate(ladder.next(), 0);  // light reference
  // Rate everything "bothers" then "fine" alternately: lo can pass hi.
  int guard = 0;
  while (!ladder.done() && guard++ < 200) {
    const Rung r = ladder.next();
    ASSERT_TRUE(ladder.rate(r, guard % 2 ? 2 : 0));
  }
  EXPECT_TRUE(ladder.done());
  for (uint8_t c = 0; c < rc::kClassCount; ++c) {
    EXPECT_LE(ladder.series(Polarity::Light, static_cast<ContentClass>(c)).rungs, rc::kMaxRungsPerSeries);
  }
}

TEST(RefreshCalibrationLadder, SkippedClassesAreNeverShown) {
  Ladder ladder(rc::classBit(ContentClass::DenseCjk));
  const auto shown = run(ladder, uniformViewer(3));
  for (const auto& r : shown) {
    EXPECT_FALSE(r.kind == RungKind::Probe && r.cls == ContentClass::DenseCjk);
  }
  EXPECT_EQ(ladder.series(Polarity::Light, ContentClass::DenseCjk).rungs, 0u);
  // The series after the skipped one warm-starts from the one before it.
  EXPECT_EQ(ladder.series(Polarity::Light, ContentClass::Sparse).start, 3);
}

TEST(RefreshCalibrationLadder, OnlyTheExpectedRungAndValidRatingsAreBooked) {
  Ladder ladder;
  Rung wrong = ladder.next();
  wrong.polarity = Polarity::Night;
  EXPECT_FALSE(ladder.rate(wrong, 0));
  EXPECT_FALSE(ladder.rate(ladder.next(), 3));
  EXPECT_EQ(ladder.rated(), 0u);
  EXPECT_TRUE(ladder.rate(ladder.next(), 0));
  EXPECT_EQ(ladder.rated(), 1u);
  const Rung probe = ladder.next();
  EXPECT_EQ(probe.kind, RungKind::Probe);
  EXPECT_EQ(probe.fastCount(), 12u);
  Rung stale = probe;
  stale.seq = 0;
  EXPECT_FALSE(ladder.rate(stale, 1));
}

TEST(RefreshCalibrationLadder, EstimatedTotalShrinksToTheExactCountAtTheEnd) {
  Ladder ladder;
  const uint8_t initial = ladder.estimatedTotal();
  EXPECT_EQ(initial, 2u + 10u * 3u);
  const auto shown = run(ladder, uniformViewer(4));
  EXPECT_EQ(ladder.estimatedTotal(), shown.size());
}

TEST(RefreshCalibrationLog, RungIdsAreShortAndReadable) {
  char id[24];
  Rung ref;
  ref.seq = 0;
  ref.formatId(id, sizeof(id));
  EXPECT_STREQ(id, "00 L-REF");
  Rung probe;
  probe.kind = RungKind::Probe;
  probe.polarity = Polarity::Night;
  probe.cls = ContentClass::GrayImage;
  probe.level = 7;
  probe.seq = 23;
  probe.formatId(id, sizeof(id));
  EXPECT_STREQ(id, "23 N-IMG-48");
}

TEST(RefreshCalibrationLog, RungRowCarriesEverythingTheAnalysisJoinsOn) {
  Ladder ladder;
  ladder.rate(ladder.next(), 0);
  const Rung r = ladder.next();
  char line[512];
  const size_t n = rc::formatRungRow(line, sizeof(line), r, sampleMeasurement(), 1, 4321);
  ASSERT_GT(n, 0u);
  EXPECT_EQ(line[n - 1], '\n');
  const auto doc = parseLine(line, n);
  EXPECT_STREQ(doc["event"].as<const char*>(), "rung");
  EXPECT_EQ(doc["seq"].as<int>(), 1);
  EXPECT_STREQ(doc["rung"].as<const char*>(), "01 L-LAT-12");
  EXPECT_STREQ(doc["kind"].as<const char*>(), "probe");
  EXPECT_STREQ(doc["polarity"].as<const char*>(), "light");
  EXPECT_STREQ(doc["class"].as<const char*>(), "dense-latin");
  EXPECT_EQ(doc["level"].as<int>(), 3);
  EXPECT_EQ(doc["fast_count"].as<int>(), 12);
  EXPECT_EQ(doc["rating"].as<int>(), 1);
  EXPECT_EQ(doc["frames"].as<int>(), 12);
  EXPECT_EQ(doc["panel_pixels"].as<long>(), 384000);
  EXPECT_EQ(doc["changed_px"].as<long long>(), 912345);
  EXPECT_EQ(doc["weighted_debt"].as<long>(), 1140431);
  EXPECT_EQ(doc["debt_permille"].as<long>(), 2969);  // 1140431 * 1000 / 384000
  EXPECT_EQ(doc["policy_clear_at"].as<int>(), 8);
  EXPECT_EQ(doc["grayscale"].as<int>(), 1);
  EXPECT_EQ(doc["image"].as<int>(), 0);
  EXPECT_EQ(doc["elapsed_ms"].as<long>(), 4321);

  // Reference rows have no class.
  Ladder fresh;
  const size_t m = rc::formatRungRow(line, sizeof(line), fresh.next(), {}, 0, 0);
  const auto ref = parseLine(line, m);
  EXPECT_STREQ(ref["kind"].as<const char*>(), "reference");
  EXPECT_TRUE(ref["class"].isNull());
  EXPECT_EQ(ref["level"].as<int>(), -1);
  EXPECT_EQ(ref["fast_count"].as<int>(), 0);
}

TEST(RefreshCalibrationLog, RowsThatDoNotFitAreRefusedNotTruncated) {
  char tiny[40];
  EXPECT_EQ(rc::formatRungRow(tiny, sizeof(tiny), Ladder().next(), sampleMeasurement(), 1, 0), 0u);
  EXPECT_EQ(tiny[0], '\0');
}

TEST(RefreshCalibrationLog, SessionRowEscapesUntrustedNamesAndListsSkips) {
  rc::SessionInfo info;
  info.session = "rc-0011aabb";
  info.firmware = "rc9.2-calib";
  info.fontName = "Evil\"Font\\";
  info.fontId = 77;
  info.pointSize = 14;
  info.antialias = true;
  info.skipMask = rc::classBit(ContentClass::DenseCjk);
  info.pixelCount = 384000;
  info.policySource = "file";
  info.config.clearThresholdPermille = 2400;
  char line[1024];
  const size_t n = rc::formatSessionRow(line, sizeof(line), info);
  ASSERT_GT(n, 0u);
  const auto doc = parseLine(line, n);
  EXPECT_STREQ(doc["event"].as<const char*>(), "session");
  EXPECT_EQ(doc["schema"].as<int>(), rc::kLogSchema);
  EXPECT_STREQ(doc["font"].as<const char*>(), "Evil?Font?");
  EXPECT_STREQ(doc["policy"].as<const char*>(), "adaptive-content-v2");
  EXPECT_STREQ(doc["policy_source"].as<const char*>(), "file");
  EXPECT_EQ(doc["policy_config"]["clear_threshold_permille"].as<int>(), 2400);
  ASSERT_EQ(doc["skip"].size(), 1u);
  EXPECT_STREQ(doc["skip"][0].as<const char*>(), "dense-cjk");
  ASSERT_EQ(doc["fast_counts"].size(), 8u);
  EXPECT_EQ(doc["fast_counts"][7].as<int>(), 48);
}

TEST(RefreshCalibrationResume, ReplayingTheLogRestoresTheExactNextRung) {
  // Run part of a session, writing the log as the activity does.
  rc::SessionInfo info;
  info.session = "rc-1";
  info.skipMask = rc::classBit(ContentClass::DenseCjk);
  std::vector<std::string> log;
  char line[1024];
  log.emplace_back(line, rc::formatSessionRow(line, sizeof(line), info));
  Ladder live(info.skipMask);
  const Viewer viewer = uniformViewer(2);
  for (int i = 0; i < 9; ++i) {
    const Rung r = live.next();
    const uint8_t rating = viewer.rate(r);
    log.emplace_back(line, rc::formatRungRow(line, sizeof(line), r, sampleMeasurement(), rating, 0));
    ASSERT_TRUE(live.rate(r, rating));
  }

  rc::Replay replay;
  for (const auto& l : log) ASSERT_EQ(replay.feed(l.data(), l.size()), rc::Replay::Status::Ok) << l;
  EXPECT_EQ(replay.ladder().skipMask(), info.skipMask);
  EXPECT_EQ(replay.ladder().rated(), 9u);
  EXPECT_EQ(replay.ladder().next(), live.next());

  // Finishing either copy gives the same result.
  Ladder resumed = replay.ladder();
  run(live, viewer);
  run(resumed, viewer);
  for (uint8_t p = 0; p < 2; ++p)
    for (uint8_t c = 0; c < rc::kClassCount; ++c) {
      const auto& a = live.series(static_cast<Polarity>(p), static_cast<ContentClass>(c));
      const auto& b = resumed.series(static_cast<Polarity>(p), static_cast<ContentClass>(c));
      EXPECT_EQ(a.lo, b.lo);
      EXPECT_EQ(a.hi, b.hi);
    }
  const size_t n = rc::formatCompleteRow(line, sizeof(line), live);
  ASSERT_GT(n, 0u);
  EXPECT_EQ(parseLine(line, n)["series"].size(), 10u);
}

TEST(RefreshCalibrationResume, DamagedOrForeignLogsAreRejected) {
  rc::Replay noHeader;
  const std::string rung = R"({"event":"rung","seq":0,"kind":"reference","polarity":"light","class":null,"level":-1,"rating":0})";
  EXPECT_EQ(noHeader.feed(rung.data(), rung.size()), rc::Replay::Status::NeedSession);

  rc::Replay replay;
  const std::string header = R"({"event":"session","schema":1,"skip":[]})";
  ASSERT_EQ(replay.feed(header.data(), header.size()), rc::Replay::Status::Ok);
  const std::string truncated = R"({"event":"rung","seq":0,"kind":"refer)";
  EXPECT_EQ(replay.feed(truncated.data(), truncated.size()), rc::Replay::Status::BadRow);
  // Out of order: the first rung of a run is the light reference, not a probe.
  const std::string skipped = R"({"event":"rung","seq":0,"kind":"probe","polarity":"light","class":"dense-latin","level":3,"rating":1})";
  EXPECT_EQ(replay.feed(skipped.data(), skipped.size()), rc::Replay::Status::Mismatch);
  EXPECT_EQ(replay.feed(rung.data(), rung.size()), rc::Replay::Status::Ok);
  const std::string blank = "\n";
  EXPECT_EQ(replay.feed(blank.data(), blank.size()), rc::Replay::Status::Ok);

  rc::Replay future;
  const std::string v2 = R"({"event":"session","schema":2,"skip":[]})";
  EXPECT_EQ(future.feed(v2.data(), v2.size()), rc::Replay::Status::BadSession);
  const std::string badSkip = R"({"event":"session","schema":1,"skip":["comics"]})";
  EXPECT_EQ(future.feed(badSkip.data(), badSkip.size()), rc::Replay::Status::BadSession);
}
