#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "AdaptiveRefreshPolicy.h"
#include "BenchmarkBuffer.h"
#include "RefreshClearEvent.h"

namespace {
using namespace benchmark;
using Policy = AdaptiveRefreshPolicy;
using Refresh = Policy::Refresh;
using Source = Policy::ClearSource;

constexpr uint32_t kPixels = 1000;
constexpr uint32_t kBook = 0x5eed1234u;

Policy::Inputs page(uint32_t changed, bool image = false, bool gray = false) {
  Policy::Inputs in;
  in.frame.pixelCount = kPixels;
  in.frame.frameValid = true;
  in.frame.historyValid = true;
  in.frame.changedPixels = changed;
  in.hasImage = image;
  in.visibleGrayscale = gray;
  return in;
}

// Stands in for ReaderActivity::recordClear: every committed clear becomes one queued row.
struct Recorder {
  Buffer buffer;
  bool policyFile = false;
  uint64_t clock = 1000;
  unsigned calls = 0;
  static void observe(void* context, const Policy::ClearRecord& record) {
    auto& self = *static_cast<Recorder*>(context);
    ++self.calls;
    self.buffer.push(refreshClearEvent(record, self.policyFile, kBook, self.clock += 1000));
  }
  std::vector<Event> drain() {
    std::vector<Event> rows;
    Event event;
    while (buffer.pop(event)) rows.push_back(event);
    return rows;
  }
};

Refresh step(Policy& policy, const Policy::Inputs& in) {
  const auto refresh = policy.decide(in).refresh;
  policy.commit(refresh);
  return refresh;
}

// A reader policy past its first frame, with the recorder attached.
Policy attached(Recorder& recorder, const Policy::Config& config = {}) {
  Policy policy(/*initialClearRequired=*/false, config);
  policy.setClearObserver(&Recorder::observe, &recorder);
  return policy;
}

std::string encode(const Event& event) {
  std::string out;
  Writer writer;
  writer.begin(
      [](void* context, const char* text, size_t length) {
        static_cast<std::string*>(context)->append(text, length);
        return length;
      },
      &out);
  EXPECT_EQ(writer.event(event), Writer::Result::Written);
  return out;
}

}  // namespace

TEST(RefreshClearRows, FastTurnsQueueNothing) {
  Recorder recorder;
  Policy policy = attached(recorder);
  for (int i = 0; i < 40; ++i) ASSERT_EQ(step(policy, page(10)), Refresh::Fast);
  EXPECT_EQ(recorder.calls, 0u);
  EXPECT_EQ(recorder.buffer.size(), 0u);
}

TEST(RefreshClearRows, DebtClearCarriesTurnsDebtPicturesAndThreshold) {
  Recorder recorder;
  Policy policy = attached(recorder);
  // 300 + 625 (grayscale 1.25x) + 400 = 1325 permille of debt after three fast turns; the
  // fourth page (+300) reaches the 1500 threshold.
  ASSERT_EQ(step(policy, page(300)), Refresh::Fast);
  ASSERT_EQ(step(policy, page(500, /*image=*/false, /*gray=*/true)), Refresh::Fast);
  ASSERT_EQ(step(policy, page(200, /*image=*/true)), Refresh::Fast);
  ASSERT_EQ(step(policy, page(300)), Refresh::Clear);
  const auto rows = recorder.drain();
  ASSERT_EQ(rows.size(), 1u);
  const Event& row = rows[0];
  EXPECT_EQ(row.type, EventType::RefreshClear);
  EXPECT_EQ(row.startUs, row.endUs);
  EXPECT_EQ(row.data[0], static_cast<uint32_t>(ClearCause::Debt));
  EXPECT_EQ(row.data[1], 3u);     // fast turns since the previous clear
  EXPECT_EQ(row.data[2], 1625u);  // 300 + 625 + 400 + 300
  EXPECT_EQ(row.data[3], 1u);     // picture pages among them
  EXPECT_EQ(row.data[4], 1u);     // grayscale pages among them
  EXPECT_EQ(row.data[5], 0u);     // light, compiled policy, plain clearing frame
  EXPECT_EQ(row.data[6], kBook);
  EXPECT_EQ(row.data[7], 1500u);
  EXPECT_EQ(encode(row),
            "{\"event\":\"refresh_clear\",\"seq\":1,\"start_us\":2000,\"end_us\":2000,\"duration_us\":0,"
            "\"data\":[2,3,1625,1,1,0,1592594996,1500]}\n");
}

TEST(RefreshClearRows, CountersRestartAfterEachClear) {
  Recorder recorder;
  Policy::Config config;
  config.safetyCap = 4;
  Policy policy = attached(recorder, config);
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < 4; ++i) ASSERT_EQ(step(policy, page(1, /*image=*/true)), Refresh::Fast);
    ASSERT_EQ(step(policy, page(1)), Refresh::Clear);
  }
  const auto rows = recorder.drain();
  ASSERT_EQ(rows.size(), 3u);
  for (const auto& row : rows) {
    EXPECT_EQ(row.type, EventType::RefreshClear);
    EXPECT_EQ(row.data[0], static_cast<uint32_t>(ClearCause::SafetyCap));
    EXPECT_EQ(row.data[1], 4u);
    EXPECT_EQ(row.data[3], 4u);
    EXPECT_EQ(row.data[4], 0u);
  }
  EXPECT_LT(rows[0].sequence, rows[1].sequence);
}

TEST(RefreshClearRows, ManualRequestsBecomeManualRefreshRows) {
  Recorder recorder;
  recorder.policyFile = true;
  Policy::Config config;
  config.nightClearThresholdPermille = 2400;
  Policy policy = attached(recorder, config);
  auto night = page(100);
  night.invertedOutput = true;
  for (int i = 0; i < 5; ++i) ASSERT_EQ(step(policy, night), Refresh::Fast);
  // A pending picture-follow request does not hide the user's command.
  policy.imagePageShown();
  policy.requestClear(Source::Manual);
  policy.requestClear(Source::Popup);
  ASSERT_EQ(step(policy, night), Refresh::Clear);
  const auto rows = recorder.drain();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].type, EventType::ManualRefresh);
  EXPECT_EQ(rows[0].data[0], static_cast<uint32_t>(ClearCause::Manual));
  EXPECT_EQ(rows[0].data[1], 5u);
  EXPECT_EQ(rows[0].data[2], 600u);
  EXPECT_EQ(rows[0].data[5], CLEAR_FLAG_NIGHT | CLEAR_FLAG_POLICY_FILE);
  EXPECT_EQ(rows[0].data[7], 2400u);
  EXPECT_NE(encode(rows[0]).find("\"event\":\"manual_refresh\""), std::string::npos);

  // The label is consumed by the clear: the next automatic clear is not manual.
  policy.requestClear(Source::Popup);
  ASSERT_EQ(step(policy, night), Refresh::Clear);
  const auto next = recorder.drain();
  ASSERT_EQ(next.size(), 1u);
  EXPECT_EQ(next[0].type, EventType::RefreshClear);
  EXPECT_EQ(next[0].data[0], static_cast<uint32_t>(ClearCause::Popup));
  EXPECT_EQ(next[0].data[1], 0u);
}

TEST(RefreshClearRows, RendererPromotionsSplitIntoManualAndOverlay) {
  Recorder recorder;
  Policy policy = attached(recorder);
  step(policy, page(50));
  auto tile = page(50);
  tile.externalClearPending = true;
  tile.externalManual = true;  // control center refresh tile
  ASSERT_EQ(step(policy, tile), Refresh::Clear);
  auto popupClose = page(50);
  popupClose.externalClearPending = true;  // option popup closing
  ASSERT_EQ(step(policy, popupClose), Refresh::Clear);
  // The EPUB placeholder path re-requests the promotion it consumed.
  policy.requestClear(Source::External);
  ASSERT_EQ(step(policy, page(50)), Refresh::Clear);
  const auto rows = recorder.drain();
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].type, EventType::ManualRefresh);
  EXPECT_EQ(rows[0].data[1], 1u);
  EXPECT_EQ(rows[1].type, EventType::RefreshClear);
  EXPECT_EQ(rows[1].data[0], static_cast<uint32_t>(ClearCause::Overlay));
  EXPECT_EQ(rows[2].data[0], static_cast<uint32_t>(ClearCause::Overlay));
}

TEST(RefreshClearRows, EveryAutomaticReasonHasItsOwnCause) {
  Recorder recorder;
  Policy policy(/*initialClearRequired=*/true);
  policy.setClearObserver(&Recorder::observe, &recorder);
  ASSERT_EQ(step(policy, page(10)), Refresh::Clear);  // initial
  step(policy, page(10, /*image=*/true));
  policy.imagePageShown();
  ASSERT_EQ(step(policy, page(10)), Refresh::Clear);  // after the picture
  policy.requestClear();
  ASSERT_EQ(step(policy, page(10)), Refresh::Clear);  // other reader request
  auto flip = page(10);
  flip.frame.polarityChangePending = true;
  ASSERT_EQ(step(policy, flip), Refresh::Clear);
  Policy::Inputs broken;  // zero-sized metrics
  ASSERT_EQ(step(policy, broken), Refresh::Clear);
  // The caller ran a clear the policy did not ask for.
  policy.decide(page(10));
  policy.commit(Refresh::Clear);
  const auto rows = recorder.drain();
  ASSERT_EQ(rows.size(), 6u);
  const ClearCause expected[] = {ClearCause::Initial, ClearCause::Image, ClearCause::Requested,
                                 ClearCause::Polarity, ClearCause::InvalidFrame, ClearCause::Unlabelled};
  for (size_t i = 0; i < rows.size(); ++i) {
    EXPECT_EQ(rows[i].type, EventType::RefreshClear) << i;
    EXPECT_EQ(rows[i].data[0], static_cast<uint32_t>(expected[i])) << i;
  }
  EXPECT_EQ(rows[1].data[1], 1u);
  EXPECT_EQ(rows[1].data[3], 1u);
  EXPECT_EQ(rows[4].data[2], 0u);  // no debt figure without valid metrics
}

TEST(RefreshClearRows, ObserverNeverChangesDecisions) {
  Recorder recorder;
  Policy plain(true);
  Policy watched(true);
  watched.setClearObserver(&Recorder::observe, &recorder);
  for (int i = 0; i < 300; ++i) {
    const auto in = page(static_cast<uint32_t>((i * 37) % 400), i % 11 == 0, i % 3 == 0);
    if (i % 29 == 0) {
      plain.requestClear();
      watched.requestClear(Source::Manual);
    }
    const auto a = plain.decide(in);
    const auto b = watched.decide(in);
    ASSERT_EQ(a.refresh, b.refresh) << i;
    ASSERT_EQ(a.reason, b.reason) << i;
    plain.commit(a.refresh);
    watched.commit(b.refresh);
    if (in.hasImage) {
      plain.imagePageShown();
      watched.imagePageShown();
    }
    ASSERT_EQ(plain.debtPixels(), watched.debtPixels()) << i;
  }
  EXPECT_GT(recorder.calls, 0u);
}

TEST(RefreshClearRows, RowsShareTheRecorderQueueCapAndFileLimit) {
  // Same 64-event queue: overflow drops new rows and counts the loss.
  Recorder recorder;
  Policy::Config config;
  config.safetyCap = 1;
  Policy policy = attached(recorder, config);
  for (size_t i = 0; i < Buffer::CAPACITY + 5; ++i) {
    step(policy, page(1));
    step(policy, page(1));
  }
  EXPECT_EQ(recorder.buffer.size(), Buffer::CAPACITY);
  EXPECT_EQ(recorder.buffer.dropped(), 5u);

  // Same 1 MiB file budget: rows stop at the limit and leave the footer reserve.
  size_t written = 0;
  Writer writer;
  writer.begin(
      [](void* context, const char*, size_t length) {
        *static_cast<size_t*>(context) += length;
        return length;
      },
      &written);
  const Event row = recorder.drain().front();
  size_t rows = 0;
  while (writer.event(row) == Writer::Result::Written) ++rows;
  EXPECT_GT(rows, 1000u);
  EXPECT_LE(writer.bytes(), Writer::MAX_BYTES - Writer::TERMINAL_RESERVE);
  EXPECT_EQ(writer.event(row), Writer::Result::Limit);
  EXPECT_TRUE(writer.active());
  Event footer;
  footer.type = EventType::End;
  EXPECT_EQ(writer.event(footer, /*terminal=*/true), Writer::Result::Written);
  EXPECT_LE(writer.bytes(), Writer::MAX_BYTES);
}
