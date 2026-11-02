#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>

#include "BatterySampling.h"
#include "BenchmarkBuffer.h"

namespace {
using namespace benchmark;

struct Sink {
  std::string output;
  size_t capacity = std::numeric_limits<size_t>::max();
  size_t calls = 0;
  bool retain = true;
  static size_t write(void* context, const char* text, size_t length) {
    auto& sink = *static_cast<Sink*>(context);
    ++sink.calls;
    const size_t accepted = std::min(length, sink.capacity);
    sink.capacity -= accepted;
    if (sink.retain) sink.output.append(text, accepted);
    return accepted;
  }
};

TEST(BenchmarkBuffer, OverflowPreservesOldestEventsAndReportsLoss) {
  Buffer buffer;
  for (size_t i = 0; i < Buffer::CAPACITY + 7; ++i) {
    Event event;
    event.data[0] = static_cast<uint32_t>(i);
    buffer.push(event);
  }
  EXPECT_EQ(buffer.size(), Buffer::CAPACITY);
  EXPECT_EQ(buffer.dropped(), 7u);
  Event event;
  for (size_t i = 0; i < Buffer::CAPACITY; ++i) {
    ASSERT_TRUE(buffer.pop(event));
    EXPECT_EQ(event.data[0], i);
    EXPECT_EQ(event.sequence, i + 1);
  }
  EXPECT_FALSE(buffer.pop(event));
  buffer.push(Event{});
  ASSERT_TRUE(buffer.pop(event));
  EXPECT_EQ(event.sequence, Buffer::CAPACITY + 8);
  EXPECT_EQ(buffer.dropped(), 7u);
}

TEST(BenchmarkBuffer, WraparoundMaintainsOrderAcrossRepeatedDrains) {
  Buffer buffer;
  Event event;
  for (uint32_t round = 0; round < 100; ++round) {
    for (uint32_t i = 0; i < 47; ++i) {
      event.data[0] = round * 47 + i;
      buffer.push(event);
    }
    for (uint32_t i = 0; i < 47; ++i) {
      ASSERT_TRUE(buffer.pop(event));
      EXPECT_EQ(event.data[0], round * 47 + i);
    }
  }
  EXPECT_EQ(buffer.dropped(), 0u);
}

TEST(BenchmarkBuffer, ResetStartsFreshSessionWithoutInheritedRowsLossOrSequence) {
  Buffer buffer;
  Event event;
  for (uint32_t i = 0; i < Buffer::CAPACITY + 9; ++i) {
    event.data[0] = 0xAAAAAAAAu;
    buffer.push(event);
  }
  ASSERT_EQ(buffer.size(), Buffer::CAPACITY);
  ASSERT_EQ(buffer.dropped(), 9u);
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(buffer.pop(event));

  buffer.reset();
  EXPECT_EQ(buffer.size(), 0u);
  EXPECT_EQ(buffer.dropped(), 0u);
  EXPECT_FALSE(buffer.pop(event));

  event = Event{};
  event.data[0] = 77;
  buffer.push(event);
  ASSERT_EQ(buffer.size(), 1u);
  ASSERT_TRUE(buffer.pop(event));
  EXPECT_EQ(event.data[0], 77u);
  EXPECT_EQ(event.sequence, 1u);
  EXPECT_FALSE(buffer.pop(event));

  // A full fresh session wraps from zero and still reports its own loss count.
  for (uint32_t i = 0; i < Buffer::CAPACITY + 3; ++i) buffer.push(Event{});
  EXPECT_EQ(buffer.size(), Buffer::CAPACITY);
  EXPECT_EQ(buffer.dropped(), 3u);
  ASSERT_TRUE(buffer.pop(event));
  EXPECT_EQ(event.sequence, 2u);
}

TEST(BenchmarkBuffer, SessionAndStopEnumsKeepAppendedSlots) {
  EXPECT_EQ(static_cast<uint8_t>(SessionMode::Normal), 0);
  EXPECT_EQ(static_cast<uint8_t>(SessionMode::Automatic), 1);
  EXPECT_EQ(static_cast<uint8_t>(StopReason::Sleep), 0);
  EXPECT_EQ(static_cast<uint8_t>(StopReason::UsbDrive), 1);
  EXPECT_EQ(static_cast<uint8_t>(StopReason::Restart), 2);
  EXPECT_EQ(static_cast<uint8_t>(StopReason::Limit), 3);
  EXPECT_EQ(static_cast<uint8_t>(StopReason::User), 4);
  EXPECT_EQ(static_cast<uint8_t>(StopReason::Complete), 5);
  EXPECT_EQ(static_cast<uint8_t>(StopReason::Cancelled), 6);
  EXPECT_EQ(static_cast<uint8_t>(StopReason::Failure), 7);
  EXPECT_EQ(static_cast<uint8_t>(EventType::RunStep), 14);
  EXPECT_EQ(static_cast<uint8_t>(EventType::RunResult), 15);
  EXPECT_EQ(static_cast<uint8_t>(EventType::SyncRun), 16);
  EXPECT_EQ(static_cast<uint8_t>(EventType::Battery), 17);
}

TEST(BenchmarkWriter, SerializesMonotonicPairsAndTypedNumericData) {
  Sink sink;
  Writer writer;
  writer.begin(Sink::write, &sink);
  Event event;
  event.type = EventType::Busy;
  event.startUs = 4294967000ULL;
  event.endUs = 4295000000ULL;
  event.sequence = 12;
  event.data[0] = 87;
  event.data[1] = 1;
  event.data[2] = 2;
  EXPECT_EQ(writer.event(event), Writer::Result::Written);
  EXPECT_EQ(sink.output,
            "{\"event\":\"busy_wait\",\"seq\":12,\"start_us\":4294967000,\"end_us\":4295000000,"
            "\"duration_us\":33000,\"data\":[87,1,2,0,0,0,0,0]}\n");
  EXPECT_EQ(writer.bytes(), sink.output.size());
}

TEST(BenchmarkWriter, SerializesRunStepAndRunResultNames) {
  Sink sink;
  Writer writer;
  writer.begin(Sink::write, &sink);
  Event event;
  event.type = EventType::RunStep;
  event.data[0] = 1;
  ASSERT_EQ(writer.event(event), Writer::Result::Written);
  event.type = EventType::RunResult;
  event.data[0] = 0;
  ASSERT_EQ(writer.event(event), Writer::Result::Written);
  EXPECT_NE(sink.output.find("\"event\":\"run_step\""), std::string::npos);
  EXPECT_NE(sink.output.find("\"event\":\"run_result\""), std::string::npos);
  EXPECT_EQ(sink.output.find("\"event\":\"display_settings\""), std::string::npos);
}

TEST(BenchmarkWriter, SerializesSyncRunAndBatterySampleWithAllEightSlots) {
  Sink sink;
  Writer writer;
  writer.begin(Sink::write, &sink);
  Event event;
  event.type = EventType::SyncRun;
  event.startUs = 1000000;
  event.endUs = 4500000;
  event.sequence = 7;
  const uint32_t sync[8] = {2, 5, 8421, 312, 3987, 3961, 71, 70};
  std::copy(std::begin(sync), std::end(sync), event.data);
  ASSERT_EQ(writer.event(event), Writer::Result::Written);
  EXPECT_EQ(sink.output,
            "{\"event\":\"sync_run\",\"seq\":7,\"start_us\":1000000,\"end_us\":4500000,"
            "\"duration_us\":3500000,\"data\":[2,5,8421,312,3987,3961,71,70]}\n");
  sink.output.clear();
  event = Event{};
  event.type = EventType::Battery;
  event.startUs = event.endUs = 9000000;
  event.data[0] = 3912;
  event.data[1] = 68;
  event.data[2] = 0;
  event.data[3] = 2;
  ASSERT_EQ(writer.event(event), Writer::Result::Written);
  EXPECT_NE(sink.output.find("\"event\":\"battery_sample\""), std::string::npos);
  EXPECT_NE(sink.output.find("\"data\":[3912,68,0,2,0,0,0,0]"), std::string::npos);
}

TEST(BenchmarkWriter, RejectsEventTypesPastTheNameTable) {
  Sink sink;
  Writer writer;
  writer.begin(Sink::write, &sink);
  Event event;
  event.type = static_cast<EventType>(static_cast<uint8_t>(EventType::Battery) + 1);
  EXPECT_EQ(writer.event(event), Writer::Result::Failed);
  EXPECT_TRUE(sink.output.empty());
}

TEST(BatterySampling, DueOnFirstSampleThenEveryFiveMinutesAwake) {
  EXPECT_EQ(BATTERY_SAMPLE_INTERVAL_MS, 300000u);
  EXPECT_TRUE(batterySampleDue(false, 0, 0));
  EXPECT_FALSE(batterySampleDue(true, 1000, 1000));
  EXPECT_FALSE(batterySampleDue(true, 1000 + BATTERY_SAMPLE_INTERVAL_MS - 1, 1000));
  EXPECT_TRUE(batterySampleDue(true, 1000 + BATTERY_SAMPLE_INTERVAL_MS, 1000));
  EXPECT_TRUE(batterySampleDue(true, 1000 + 3 * BATTERY_SAMPLE_INTERVAL_MS, 1000));
}

TEST(BatterySampling, SurvivesMillisWraparound) {
  const uint32_t last = std::numeric_limits<uint32_t>::max() - 1000;
  EXPECT_FALSE(batterySampleDue(true, 5000, last));
  EXPECT_TRUE(batterySampleDue(true, BATTERY_SAMPLE_INTERVAL_MS, last));
}

TEST(BenchmarkWriter, ShortWriteDisablesWithoutRetryingOrOverwritingPrefix) {
  Sink sink;
  sink.capacity = 7;
  Writer writer;
  writer.begin(Sink::write, &sink);
  EXPECT_EQ(writer.line("first-record\n", 13), Writer::Result::Failed);
  EXPECT_EQ(sink.output, "first-r");
  EXPECT_EQ(writer.bytes(), 7u);
  EXPECT_FALSE(writer.active());
  EXPECT_EQ(writer.event(Event{}), Writer::Result::Failed);
  EXPECT_EQ(writer.line("replacement\n", 12, true), Writer::Result::Failed);
  EXPECT_EQ(sink.calls, 1u);
  EXPECT_EQ(sink.output, "first-r");
}

TEST(BenchmarkWriter, FullCardFailsWithoutRetryLoop) {
  Sink sink;
  sink.capacity = 0;
  Writer writer;
  writer.begin(Sink::write, &sink);
  EXPECT_EQ(writer.event(Event{}), Writer::Result::Failed);
  EXPECT_FALSE(writer.active());
  EXPECT_EQ(writer.bytes(), 0u);
  EXPECT_EQ(sink.calls, 1u);
}

TEST(BenchmarkWriter, LimitReservesTerminalSpaceAndNeverExceedsCap) {
  Sink sink;
  sink.retain = false;
  Writer writer;
  writer.begin(Sink::write, &sink);
  std::array<char, 1024> block{};
  const size_t normalLimit = Writer::MAX_BYTES - Writer::TERMINAL_RESERVE;
  while (writer.bytes() < normalLimit) {
    ASSERT_EQ(writer.line(block.data(), block.size()), Writer::Result::Written);
  }
  const size_t calls = sink.calls;
  EXPECT_EQ(writer.line("x", 1), Writer::Result::Limit);
  EXPECT_EQ(sink.calls, calls);
  Event end;
  end.type = EventType::End;
  end.data[0] = static_cast<uint32_t>(StopReason::Limit);
  EXPECT_EQ(writer.event(end, true), Writer::Result::Written);
  const size_t remaining = Writer::MAX_BYTES - writer.bytes();
  EXPECT_EQ(writer.line(block.data(), remaining, true), Writer::Result::Written);
  EXPECT_EQ(writer.line("x", 1, true), Writer::Result::Limit);
  EXPECT_EQ(writer.bytes(), Writer::MAX_BYTES);
}

TEST(BenchmarkWriter, ExplicitDisablePreventsFurtherSinkAccess) {
  Sink sink;
  Writer writer;
  writer.begin(Sink::write, &sink);
  ASSERT_EQ(writer.event(Event{}), Writer::Result::Written);
  writer.disable();
  const auto before = sink.output;
  EXPECT_EQ(writer.event(Event{}, true), Writer::Result::Failed);
  EXPECT_EQ(sink.calls, 1u);
  EXPECT_EQ(sink.output, before);
}

TEST(BenchmarkPrivacy, EventStoresIdsNotSourceStrings) {
  const char* privatePath = "/Books/private title.epub";
  Sink sink;
  Writer writer;
  writer.begin(Sink::write, &sink);
  Event event;
  event.data[0] = id(privatePath);
  ASSERT_EQ(writer.event(event), Writer::Result::Written);
  EXPECT_EQ(sink.output.find("Books"), std::string::npos);
  EXPECT_EQ(sink.output.find("private"), std::string::npos);
  EXPECT_EQ(id("hello"), 0x4f9f2cabu);  // Portable FNV-1a, not implementation-defined std::hash.
  EXPECT_EQ(id(privatePath), event.data[0]);
}

TEST(BenchmarkPrivacy, MetadataEscapesControlCharactersAndRejectsTruncation) {
  char escaped[64];
  EXPECT_GT(jsonString(escaped, sizeof(escaped), "build\"\\\n\t"), 0u);
  EXPECT_STREQ(escaped, "\"build\\\"\\\\\\u000a\\u0009\"");
  char shortOutput[5];
  EXPECT_EQ(jsonString(shortOutput, sizeof(shortOutput), "abcdef"), 0u);
  EXPECT_EQ(jsonString(shortOutput, sizeof(shortOutput), nullptr), 0u);
}

TEST(BenchmarkWriter, RejectsInvalidTimePairsRatherThanUnderflowing) {
  Sink sink;
  Writer writer;
  writer.begin(Sink::write, &sink);
  Event event;
  event.startUs = 100;
  event.endUs = 99;
  EXPECT_EQ(writer.event(event), Writer::Result::Failed);
  EXPECT_EQ(sink.calls, 0u);
}
}  // namespace
