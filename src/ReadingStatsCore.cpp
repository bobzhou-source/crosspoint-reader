#include "ReadingStatsCore.h"

#include <cstdio>
#include <iterator>

namespace reading_stats {
uint32_t civilDay(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned y = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  return static_cast<uint32_t>(era * 146097 + y * 365 + y / 4 - y / 100 + doy - 719468);
}
void formatDay(uint32_t day, char* out, size_t size) {
  if (!day) {
    snprintf(out, size, "--");
    return;
  }
  const unsigned z = day + 719468;
  const unsigned era = z / 146097;
  const unsigned doe = z - era * 146097;
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  unsigned y = yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
  snprintf(out, size, "%04u-%02u-%02u", y, m, d);
}
uint64_t bookId(const char* path) {
  uint64_t hash = 14695981039346656037ULL;
  while (*path) {
    hash ^= static_cast<unsigned char>(*path++);
    hash *= 1099511628211ULL;
  }
  return hash;
}
uint32_t checksum(const void* data, size_t size) {
  uint32_t crc = 0xffffffff;
  const auto* p = static_cast<const uint8_t*>(data);
  while (size--) {
    crc ^= *p++;
    for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}
Day& Accumulator::daily(uint32_t day) {
  auto& result = summary.days[day % 30];
  if (result.date != day) {
    result = {};
    result.date = day;
  }
  return result;
}
void Accumulator::touchDate(uint32_t day) {
  if (!day) return;
  if (!book.firstDay || day < book.firstDay) book.firstDay = day;
  book.lastDay = day;
}
void Accumulator::begin(uint32_t now, uint32_t day, uint32_t second, uint32_t progress) {
  book.position = std::min<uint32_t>(progress, 10000U);
  ++book.totals.sessions;
  if (day) ++daily(day).totals.sessions;
  touchDate(day);
  dirty = true;
  resume(now, day, second);
}
void Accumulator::resume(uint32_t now, uint32_t day, uint32_t second) {
  running = true;
  lastTick = lastInput = now;
  previousDay = day;
  previousSecond = second;
}
void Accumulator::tick(uint32_t now, uint32_t day, uint32_t second, bool input) {
  if (!running) return;
  const uint32_t elapsed = now - lastTick;
  const uint32_t idleBefore = lastTick - lastInput;
  const uint32_t credit = idleBefore >= IDLE_MS ? 0 : std::min(elapsed, IDLE_MS - idleBefore);
  if (credit) {
    book.totals.milliseconds += credit;
    // A normal midnight crossing splits the interval at local midnight. Clock
    // corrections never change elapsed time and never fabricate old daily time.
    if (previousDay && day == previousDay + 1) {
      const uint32_t beforeMidnight = (86400 - std::min<uint32_t>(previousSecond, 86399U)) * 1000;
      const uint32_t before = std::min(credit, beforeMidnight);
      daily(previousDay).totals.milliseconds += before;
      daily(day).totals.milliseconds += credit - before;
    } else if (day) {
      daily(day).totals.milliseconds += credit;
    }
    touchDate(day);
    dirty = true;
  }
  lastTick = now;
  previousDay = day;
  previousSecond = second;
  if (input) lastInput = now;
}
void Accumulator::pause(uint32_t now, uint32_t day, uint32_t second) {
  tick(now, day, second, false);
  running = false;
}
void Accumulator::page(uint32_t progress) {
  progress = std::min<uint32_t>(progress, 10000U);
  ++book.totals.pages;
  if (previousDay) ++daily(previousDay).totals.pages;
  if (progress > book.position) book.advanced += progress - book.position;
  book.position = progress;
  dirty = true;
}
Totals Accumulator::recent(uint32_t day, unsigned count) const {
  Totals result;
  if (!day) return result;
  for (const auto& entry : summary.days) {
    if (entry.date && entry.date <= day && day - entry.date < std::min(count, 30U)) {
      result.milliseconds += entry.totals.milliseconds;
      result.pages += entry.totals.pages;
      result.sessions += entry.totals.sessions;
    }
  }
  return result;
}
namespace {
// A day counts toward the habit only when it holds real reading: a session
// opened and closed without a page turn or a credited idle interval is not a
// habit day. Ring slots are checked against their stored date so a stale slot
// from 30 days ago can never be mistaken for today.
bool activeDay(const Summary& summary, const uint32_t day) {
  if (!day) return false;
  const Day& entry = summary.days[day % 30];
  return entry.date == day && (entry.totals.pages || entry.totals.milliseconds);
}
}  // namespace

Habit Accumulator::habit(const uint32_t today) const {
  Habit result;
  if (!today) return result;
  if (const Day& entry = summary.days[today % 30]; entry.date == today) {
    result.todayPagesRead = entry.totals.pages;
    result.todayMinutesRead = static_cast<uint32_t>(entry.totals.milliseconds / 60000);
  }
  // The ring holds 30 days, so a streak is reported within that window; a
  // longer run reads as 30+ days rather than a wrong number.
  for (uint32_t back = 0; back < 30; ++back) {
    const uint32_t day = today - back;
    if (!day || !activeDay(summary, day)) break;
    ++result.currentStreakDays;
  }
  // Today is not over: a run that ended yesterday is still the live streak, so
  // the badge does not drop to zero while the reader has not started yet. The
  // 30-day ring bounds this walk; a slot from 30 days ago cannot masquerade as
  // yesterday because activeDay() checks the stored date.
  if (!result.currentStreakDays && activeDay(summary, today - 1)) {
    for (uint32_t back = 1; back <= 30; ++back) {
      const uint32_t day = today - back;
      if (!day || !activeDay(summary, day)) break;
      ++result.currentStreakDays;
    }
  }
  for (uint32_t back = 0; back < 30; ++back) {
    const uint32_t day = today - back;
    if (!day || !activeDay(summary, day)) continue;
    // Count each run once, from its oldest day.
    if (day > 1 && activeDay(summary, day - 1)) continue;
    uint32_t length = 0;
    for (uint32_t forward = day; forward <= today && activeDay(summary, forward); ++forward) ++length;
    result.bestStreakDays = std::max(result.bestStreakDays, length);
  }
  return result;
}

void Accumulator::updateTop() {
  size_t index = TOP_COUNT - 1;
  for (size_t i = 0; i < TOP_COUNT; ++i) {
    if (summary.top[i].id == book.id) {
      index = i;
      break;
    }
  }
  if (summary.top[index].id != book.id && summary.top[index].milliseconds > book.totals.milliseconds) return;
  auto& entry = summary.top[index];
  entry.id = book.id;
  entry.milliseconds = book.totals.milliseconds;
  memcpy(entry.title, book.title, sizeof(entry.title));
  std::sort(std::begin(summary.top), std::end(summary.top),
            [](const Top& a, const Top& b) { return a.milliseconds > b.milliseconds; });
}
static bool valid(const Header& h, const void* data, size_t size) {
  return h.magic == 0x52535438 && h.version == 1 && h.bytes == size && h.generation && h.crc == checksum(data, size);
}
bool load(Files& files, const char* stem, void* data, size_t size, uint32_t& generation) {
  char path[128];
  Header h;
  uint32_t generations[2] = {};
  for (unsigned slot = 0; slot < 2; ++slot) {
    snprintf(path, sizeof(path), "%s.%u", stem, slot);
    if (files.read(path, h, data, size) && valid(h, data, size)) generations[slot] = h.generation;
  }
  const unsigned best = generations[1] > generations[0] ? 1 : 0;
  generation = generations[best];
  if (generation) {
    snprintf(path, sizeof(path), "%s.%u", stem, best);
    if (files.read(path, h, data, size) && valid(h, data, size)) return true;
  }
  memset(data, 0, size);
  generation = 0;
  return false;
}
bool save(Files& files, const char* stem, const void* data, size_t size, uint32_t& generation) {
  Header h;
  h.bytes = size;
  h.generation = generation + 1;
  h.crc = checksum(data, size);
  char path[128];
  snprintf(path, sizeof(path), "%s.%u", stem, h.generation % 2);
  if (!files.commit(path, h, data, size)) return false;
  generation = h.generation;
  return true;
}
}  // namespace reading_stats
