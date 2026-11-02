#include <gtest/gtest.h>

#include <TxtBackgroundIndexing.h>
#include <TxtPagination.h>

#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "TxtReplay.h"

namespace {

struct InterruptMeasure {
  WidthOracle oracle{WidthOracle::Model::Simple};
  bool inputPending = false;
  size_t interruptAtWidth = std::numeric_limits<size_t>::max();
  size_t interruptAtChunk = std::numeric_limits<size_t>::max();
  size_t widths = 0;
  std::vector<std::string> prepared;

  static bool shouldYield(void* ctx) { return static_cast<InterruptMeasure*>(ctx)->inputPending; }

  static void prepareChunk(void* ctx, const char* text) {
    auto& self = *static_cast<InterruptMeasure*>(ctx);
    self.prepared.emplace_back(text);
    if (self.prepared.size() >= self.interruptAtChunk) self.inputPending = true;
  }

  static void preparePage(void* ctx, const char* text) {
    TxtBackgroundIndexing::prepareInChunks(text, ctx, prepareChunk, shouldYield);
  }

  static int advance(void* ctx, const char* text) {
    auto& self = *static_cast<InterruptMeasure*>(ctx);
    if (++self.widths >= self.interruptAtWidth) self.inputPending = true;
    return self.oracle.width(text);
  }

  TxtPagination::PageLayout layout(const int lines = 10) {
    return {440, lines, {this, preparePage, advance, nullptr, shouldYield}, true, 20};
  }
};

}  // namespace

TEST(TxtBackgroundIndexing, HoldsOffForOneHundredMillisecondsAfterInput) {
  TxtBackgroundIndexing::QuietGate gate;
  EXPECT_TRUE(gate.ready(1000, false));
  // Raw contact, held state and release all prolong the quiet interval.
  EXPECT_FALSE(gate.ready(1001, true));
  EXPECT_FALSE(gate.ready(1030, true));
  EXPECT_FALSE(gate.ready(1031, false));
  EXPECT_FALSE(gate.ready(1040, true));
  EXPECT_FALSE(gate.ready(1139, false));
  EXPECT_TRUE(gate.ready(1140, false));
  EXPECT_TRUE(gate.ready(1141, false));
}

TEST(TxtBackgroundIndexing, HeldInputCannotStartWorkEvenAfterTheQuietInterval) {
  TxtBackgroundIndexing::QuietGate gate;
  EXPECT_FALSE(gate.ready(1000, true));
  EXPECT_FALSE(gate.ready(10000, true));
  EXPECT_FALSE(gate.ready(10099, false));
  EXPECT_TRUE(gate.ready(10100, false));
}

TEST(TxtBackgroundIndexing, QuietIntervalSurvivesClockWrap) {
  TxtBackgroundIndexing::QuietGate gate;
  EXPECT_FALSE(gate.ready(UINT32_MAX - 50, true));
  EXPECT_FALSE(gate.ready(48, false));
  EXPECT_TRUE(gate.ready(49, false));
}

TEST(TxtBackgroundIndexing, MetricChunksPreserveUtf8AndEverySourceByte) {
  for (const std::string& cp : {std::string("é"), std::string("中"), std::string("😀")}) {
    for (size_t prefix = 124; prefix <= 129; prefix++) {
      SCOPED_TRACE(prefix);
      std::string text(prefix, 'a');
      for (size_t i = 0; i < 200; i++) text += cp;
      InterruptMeasure measure;
      ASSERT_TRUE(TxtBackgroundIndexing::prepareInChunks(text.c_str(), &measure, InterruptMeasure::prepareChunk,
                                                         InterruptMeasure::shouldYield));
      std::string joined;
      for (const std::string& chunk : measure.prepared) {
        ASSERT_FALSE(chunk.empty());
        EXPECT_LE(chunk.size(), TxtBackgroundIndexing::PREPARE_BYTES);
        EXPECT_EQ(TxtLineBreak::completePrefix(chunk.data(), chunk.size()), chunk.size());
        joined += chunk;
      }
      EXPECT_EQ(joined, text);
    }
  }
}

TEST(TxtBackgroundIndexing, FontPreparationStopsAtTheNextChunkAndCanBeRetried) {
  const std::string text(8192, 'a');
  InterruptMeasure measure;
  measure.interruptAtChunk = 2;
  EXPECT_FALSE(TxtBackgroundIndexing::prepareInChunks(text.c_str(), &measure, InterruptMeasure::prepareChunk,
                                                      InterruptMeasure::shouldYield));
  ASSERT_EQ(measure.prepared.size(), 2u);
  EXPECT_EQ(measure.prepared.front().size(), TxtBackgroundIndexing::PREPARE_BYTES);
  measure.prepared.clear();
  measure.inputPending = false;
  measure.interruptAtChunk = std::numeric_limits<size_t>::max();
  EXPECT_TRUE(TxtBackgroundIndexing::prepareInChunks(text.c_str(), &measure, InterruptMeasure::prepareChunk,
                                                     InterruptMeasure::shouldYield));
  std::string joined;
  for (const auto& chunk : measure.prepared) joined += chunk;
  EXPECT_EQ(joined, text);
}

TEST(TxtBackgroundIndexing, PendingInputDoesNotReadOrPrepareThePage) {
  const std::string text = "第一章\n正文正文正文\n";
  txt_replay::installSource(text);
  TxtPagination::SourceWindow window;
  window.setSource(txt_replay::kSourcePath);
  InterruptMeasure measure;
  measure.inputPending = true;
  const auto result = TxtPagination::loadPage(window, 0, text.size(), measure.layout(), nullptr);
  EXPECT_EQ(result.status, TxtPagination::ReadStatus::Interrupted);
  EXPECT_EQ(result.nextOffset, 0u);
  EXPECT_EQ(result.lineCount, 0);
  EXPECT_EQ(Storage.source().readCalls, 0);
  EXPECT_TRUE(measure.prepared.empty());
  EXPECT_EQ(measure.widths, 0u);
}

TEST(TxtBackgroundIndexing, InputDuringFontPreparationNeverPublishesAPartialPage) {
  const std::string text = "第一章\n" + txt_replay::cjkText(42, 20000, 600);
  txt_replay::installSource(text);
  TxtPagination::SourceWindow window;
  window.setSource(txt_replay::kSourcePath);
  InterruptMeasure measure;
  measure.interruptAtChunk = 1;
  TxtPagination::PageIndexBuilder builder;
  builder.reset(text.size());
  const auto result = builder.step(window, measure.layout(), 1);
  EXPECT_EQ(result.status, TxtPagination::IndexStatus::Interrupted);
  EXPECT_EQ(result.read, TxtPagination::ReadStatus::Interrupted);
  EXPECT_EQ(result.offset, 0u);
  EXPECT_EQ(builder.frontier(), 0u);
  EXPECT_EQ(builder.offsets(), std::vector<uint32_t>{0});
  EXPECT_TRUE(builder.chapters().empty());
  EXPECT_FALSE(builder.complete());
  EXPECT_EQ(measure.widths, 0u);
  ASSERT_EQ(measure.prepared.size(), 1u);
}

TEST(TxtBackgroundIndexing, InputDuringLayoutRollsBackOnlyTheUnfinishedPage) {
  const std::string text = "第一章\n第二章\n第三章\n正文正文正文\n";
  txt_replay::installSource(text);
  TxtPagination::SourceWindow window;
  window.setSource(txt_replay::kSourcePath);
  InterruptMeasure measure;
  const auto layout = measure.layout(1);
  TxtPagination::PageIndexBuilder builder;
  builder.reset(text.size());
  ASSERT_EQ(builder.step(window, layout, 1).status, TxtPagination::IndexStatus::Ok);
  const auto beforeOffsets = builder.offsets();
  const auto beforeChapters = builder.chapters();
  ASSERT_EQ(beforeChapters, std::vector<uint32_t>{0});
  ASSERT_EQ(beforeOffsets.size(), 2u);

  // The second chapter has been collected when its first line's measurement sees input.
  measure.interruptAtWidth = measure.widths + 1;
  const auto interrupted = builder.step(window, layout, 1);
  EXPECT_EQ(interrupted.status, TxtPagination::IndexStatus::Interrupted);
  EXPECT_EQ(interrupted.offset, beforeOffsets.back());
  EXPECT_EQ(builder.offsets(), beforeOffsets);
  EXPECT_EQ(builder.chapters(), beforeChapters);
  EXPECT_FALSE(builder.complete());

  measure.inputPending = false;
  measure.interruptAtWidth = std::numeric_limits<size_t>::max();
  while (!builder.complete()) ASSERT_EQ(builder.step(window, layout, 1).status, TxtPagination::IndexStatus::Ok);
  const auto resumedOffsets = builder.offsets();
  const auto resumedChapters = builder.chapters();

  InterruptMeasure uninterrupted;
  auto foreground = uninterrupted.layout(1);
  foreground.measure.shouldYield = nullptr;
  std::vector<uint32_t> fullOffsets;
  std::vector<uint32_t> fullChapters;
  ASSERT_EQ(TxtPagination::buildPageIndex(window, text.size(), foreground, fullOffsets, &fullChapters).status,
            TxtPagination::IndexStatus::Ok);
  EXPECT_EQ(resumedOffsets, fullOffsets);
  EXPECT_EQ(resumedChapters, fullChapters);
  EXPECT_EQ(resumedChapters.size(), 3u);
}

TEST(TxtBackgroundIndexing, InterruptionRestoresTemporarilyTerminatedSourceBytes) {
  std::string text = "第一章\n正文正文正文正文正文正文正文\n";
  const std::string original = text;
  InterruptMeasure measure;
  measure.interruptAtWidth = 1;
  size_t next = 99;
  EXPECT_EQ(TxtPagination::layoutPage(text.data(), text.size(), 0, text.size(), true, measure.layout(), nullptr,
                                     nullptr, next), -1);
  EXPECT_EQ(next, 0u);
  EXPECT_EQ(text, original);
  EXPECT_EQ(measure.widths, 1u);
}

TEST(TxtBackgroundIndexing, ForegroundLayoutDoesNotYieldForInput) {
  const std::string text = "第一章\n第二章\n第三章\n";
  txt_replay::installSource(text);
  TxtPagination::SourceWindow window;
  window.setSource(txt_replay::kSourcePath);
  InterruptMeasure measure;
  measure.inputPending = true;
  auto layout = measure.layout();
  layout.measure.preparePage = InterruptMeasure::prepareChunk;
  layout.measure.shouldYield = nullptr;
  TxtPagination::PageIndexBuilder builder;
  builder.reset(text.size());
  const auto result = builder.step(window, layout, 1);
  EXPECT_EQ(result.status, TxtPagination::IndexStatus::Ok);
  EXPECT_TRUE(builder.complete());
  EXPECT_EQ(builder.chapters().size(), 3u);
  EXPECT_GT(measure.widths, 0u);
}

TEST(TxtBackgroundIndexing, ColdCodepointMeasurementYieldsBeforeTheNextGlyph) {
  std::string text = "第一章正文正文正文正文正文正文正文";
  const std::string original = text;
  InterruptMeasure measure;
  measure.interruptAtWidth = 1;
  auto layout = measure.layout();
  layout.measure.preparePage = nullptr;
  layout.measure.codepointAdvance = [](void* ctx, uint32_t) {
    return InterruptMeasure::advance(ctx, "中");
  };
  size_t next = 99;
  EXPECT_EQ(TxtPagination::layoutPage(text.data(), text.size(), 0, text.size(), true, layout, nullptr, nullptr, next),
            -1);
  EXPECT_EQ(measure.widths, 1u);
  EXPECT_EQ(next, 0u);
  EXPECT_EQ(text, original);
}
