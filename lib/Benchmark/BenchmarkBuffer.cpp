#include "BenchmarkBuffer.h"

#include <cstdio>
#include <cstring>
#include <limits>

namespace benchmark {

void Buffer::push(const Event& event) {
  const uint32_t sequence = nextSequence++;
  if (count == CAPACITY) {
    if (lost != std::numeric_limits<uint32_t>::max()) ++lost;
    return;
  }
  auto& destination = events[(first + count) % CAPACITY];
  destination = event;
  destination.sequence = sequence;
  ++count;
}

bool Buffer::pop(Event& event) {
  if (!count) return false;
  event = events[first];
  first = (first + 1) % CAPACITY;
  --count;
  return true;
}

void Writer::begin(Write write, void* ctx) {
  sink = write;
  context = ctx;
  written = 0;
  live = sink != nullptr;
}

Writer::Result Writer::line(const char* text, size_t length, bool terminal) {
  if (!live) return Result::Failed;
  const size_t budget = terminal ? MAX_BYTES : MAX_BYTES - TERMINAL_RESERVE;
  if (written > budget || length > budget - written) return Result::Limit;
  const size_t actual = sink(context, text, length);
  written += actual;
  if (actual != length) {
    live = false;
    return Result::Failed;
  }
  return Result::Written;
}

Writer::Result Writer::event(const Event& event, bool terminal) {
  static constexpr const char* NAMES[] = {"activity", "render", "display_call", "busy_wait", "memory", "index",
                                          "epub_phase", "reader_input", "sleep_request", "loss", "flush", "end",
                                          "reader_settings", "display_settings", "run_step", "run_result",
                                          "sync_run", "battery_sample", "nav_step", "nav_result", "nav_input",
                                          "nav_rows", "nav_restore", "reserved_23", "stats_flush"};
  const auto type = static_cast<size_t>(event.type);
  if (type >= sizeof(NAMES) / sizeof(NAMES[0]) || event.endUs < event.startUs) {
    live = false;
    return Result::Failed;
  }
  const int length = snprintf(encoded, sizeof(encoded),
                              "{\"event\":\"%s\",\"seq\":%u,\"start_us\":%llu,\"end_us\":%llu,"
                              "\"duration_us\":%llu,\"data\":[%u,%u,%u,%u,%u,%u,%u,%u]}\n",
                              NAMES[type], static_cast<unsigned>(event.sequence),
                              static_cast<unsigned long long>(event.startUs),
                              static_cast<unsigned long long>(event.endUs),
                              static_cast<unsigned long long>(event.endUs - event.startUs),
                              static_cast<unsigned>(event.data[0]), static_cast<unsigned>(event.data[1]),
                              static_cast<unsigned>(event.data[2]), static_cast<unsigned>(event.data[3]),
                              static_cast<unsigned>(event.data[4]), static_cast<unsigned>(event.data[5]),
                              static_cast<unsigned>(event.data[6]), static_cast<unsigned>(event.data[7]));
  if (length < 0 || static_cast<size_t>(length) >= sizeof(encoded)) {
    live = false;
    return Result::Failed;
  }
  return line(encoded, static_cast<size_t>(length), terminal);
}

Event encodeStatsFlush(const uint64_t startUs, const uint64_t endUs, const StatsFlushReason reason,
                       const StatsFlushResult result, const uint32_t bookGeneration, const uint32_t summaryGeneration,
                       const uint32_t sincePreviousMs, const uint32_t lateMs, const bool dirty) {
  Event event;
  event.type = EventType::StatsFlush;
  event.startUs = startUs;
  event.endUs = endUs < startUs ? startUs : endUs;
  event.data[0] = static_cast<uint32_t>(reason);
  event.data[1] = static_cast<uint32_t>(result);
  event.data[2] = bookGeneration;
  event.data[3] = summaryGeneration;
  event.data[4] = sincePreviousMs;
  event.data[5] = lateMs;
  event.data[6] = dirty ? 1u : 0u;
  return event;
}

uint32_t id(const char* text) {
  uint32_t hash = 2166136261u;
  if (text) {
    while (*text) {
      hash = (hash ^ static_cast<uint8_t>(*text++)) * 16777619u;
    }
  }
  return hash;
}

size_t jsonString(char* output, size_t capacity, const char* input) {
  if (capacity < 3 || !input) return 0;
  size_t used = 0;
  output[used++] = '"';
  static constexpr char HEX[] = "0123456789abcdef";
  while (*input) {
    const auto c = static_cast<uint8_t>(*input++);
    const size_t needed = c < 0x20 ? 6 : (c == '"' || c == '\\' ? 2 : 1);
    if (needed + 2 > capacity - used) return 0;
    if (c < 0x20) {
      output[used++] = '\\';
      output[used++] = 'u';
      output[used++] = '0';
      output[used++] = '0';
      output[used++] = HEX[c >> 4];
      output[used++] = HEX[c & 15];
    } else {
      if (needed == 2) output[used++] = '\\';
      output[used++] = static_cast<char>(c);
    }
  }
  output[used++] = '"';
  output[used] = '\0';
  return used;
}

}  // namespace benchmark
