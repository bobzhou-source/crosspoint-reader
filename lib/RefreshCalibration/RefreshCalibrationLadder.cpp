#include "RefreshCalibrationLadder.h"

#include <ArduinoJson.h>
#include <RefreshPolicyConfig.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace refresh_calibration {
namespace {

constexpr const char* kClassNames[kClassCount] = {"dense-latin", "dense-cjk", "sparse", "gray-image", "menu"};
constexpr const char* kClassCodes[kClassCount] = {"LAT", "CJK", "SPR", "IMG", "MNU"};

// Bounded JSON line builder: any overflow poisons the whole line.
struct Line {
  char* out;
  size_t capacity;
  size_t length = 0;
  bool ok = true;

  void add(const char* format, ...) __attribute__((format(printf, 2, 3))) {
    if (!ok) return;
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(out + length, capacity - length, format, args);
    va_end(args);
    if (n < 0 || static_cast<size_t>(n) >= capacity - length) {
      ok = false;
      return;
    }
    length += static_cast<size_t>(n);
  }

  // Quoted JSON string; characters JSON would need escaped become '?'.
  void addString(const char* text) {
    add("\"");
    for (const char* p = text ? text : ""; ok && *p; ++p) {
      const char c = *p;
      add("%c", (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) ? '?' : c);
    }
    add("\"");
  }

  size_t finish() {
    add("\n");
    if (!ok) {
      if (capacity) out[0] = '\0';
      return 0;
    }
    return length;
  }
};

}  // namespace

const char* polarityName(const Polarity polarity) { return polarity == Polarity::Night ? "night" : "light"; }

const char* className(const ContentClass cls) {
  const auto i = static_cast<uint8_t>(cls);
  return i < kClassCount ? kClassNames[i] : "unknown";
}

const char* classCode(const ContentClass cls) {
  const auto i = static_cast<uint8_t>(cls);
  return i < kClassCount ? kClassCodes[i] : "???";
}

bool parsePolarity(const char* name, Polarity& out) {
  if (!name) return false;
  if (strcmp(name, "light") == 0) {
    out = Polarity::Light;
    return true;
  }
  if (strcmp(name, "night") == 0) {
    out = Polarity::Night;
    return true;
  }
  return false;
}

bool parseClass(const char* name, ContentClass& out) {
  if (!name) return false;
  for (uint8_t i = 0; i < kClassCount; ++i) {
    if (strcmp(name, kClassNames[i]) == 0) {
      out = static_cast<ContentClass>(i);
      return true;
    }
  }
  return false;
}

void Rung::formatId(char* out, const size_t capacity) const {
  const char p = polarity == Polarity::Night ? 'N' : 'L';
  if (kind == RungKind::Reference) {
    snprintf(out, capacity, "%02u %c-REF", static_cast<unsigned>(seq), p);
  } else {
    snprintf(out, capacity, "%02u %c-%s-%u", static_cast<unsigned>(seq), p, classCode(cls),
             static_cast<unsigned>(fastCount()));
  }
}

Ladder::Ladder(const uint8_t skipMask) : skipMask_(skipMask) { skipFinishedPhases(); }

Series& Ladder::seriesAt(const Polarity polarity, const ContentClass cls) {
  return series_[static_cast<uint8_t>(polarity)][static_cast<uint8_t>(cls)];
}

const Series& Ladder::series(const Polarity polarity, const ContentClass cls) const {
  return series_[static_cast<uint8_t>(polarity)][static_cast<uint8_t>(cls)];
}

int8_t Ladder::startLevelFor(const Polarity polarity, const ContentClass cls) const {
  // Same content in daylight is the closest predictor of a night series.
  if (polarity == Polarity::Night && !(skipMask_ & classBit(cls))) {
    const Series& day = series(Polarity::Light, cls);
    if (day.rungs) return day.boundary();
  }
  // Otherwise the previous finished series of this polarity.
  for (int c = static_cast<int>(cls) - 1; c >= 0; --c) {
    const auto previous = static_cast<ContentClass>(c);
    if (skipMask_ & classBit(previous)) continue;
    const Series& s = series(polarity, previous);
    if (s.rungs) return s.boundary();
  }
  return kFirstStartLevel;
}

void Ladder::skipFinishedPhases() {
  while (phase_ < kPhaseCount && !phaseIsReference(phase_)) {
    const ContentClass cls = phaseClass(phase_);
    if (!(skipMask_ & classBit(cls))) {
      Series& s = seriesAt(phasePolarity(phase_), cls);
      if (s.rungs == 0) s.start = startLevelFor(phasePolarity(phase_), cls);
      if (!s.done()) return;
    }
    ++phase_;
  }
}

Rung Ladder::next() const {
  Rung rung;
  rung.seq = rated_;
  if (done()) return rung;
  rung.polarity = phasePolarity(phase_);
  if (phaseIsReference(phase_)) return rung;
  rung.kind = RungKind::Probe;
  rung.cls = phaseClass(phase_);
  const Series& s = series(rung.polarity, rung.cls);
  if (s.rungs == 0) {
    rung.level = s.start;
  } else if (s.lo >= 0 && s.hi < kLevels) {
    rung.level = static_cast<int8_t>((s.lo + s.hi) / 2);
  } else if (s.lo >= 0) {
    const int level = s.lo + (1 << s.widen);
    rung.level = static_cast<int8_t>(level < kLevels ? level : kLevels - 1);
  } else {
    const int level = s.hi - (1 << s.widen);
    rung.level = static_cast<int8_t>(level > 0 ? level : 0);
  }
  return rung;
}

bool Ladder::rate(const Rung& rung, const uint8_t rating) {
  if (done() || rating > kRatingBothers || !(rung == next())) return false;
  ++rated_;
  if (rung.kind == RungKind::Reference) {
    ++phase_;
    skipFinishedPhases();
    return true;
  }
  Series& s = seriesAt(rung.polarity, rung.cls);
  ++s.rungs;
  if (rating == kRatingBothers) {
    if (rung.level < s.hi) s.hi = rung.level;
  } else if (rung.level > s.lo) {
    s.lo = rung.level;
  }
  const bool bracketed = s.lo >= 0 && s.hi < kLevels;
  if (!bracketed && s.rungs > 1) ++s.widen;
  if (s.done()) {
    ++phase_;
    skipFinishedPhases();
  }
  return true;
}

uint8_t Ladder::estimatedTotal() const {
  unsigned total = rated_;
  for (uint8_t phase = phase_; phase < kPhaseCount; ++phase) {
    if (phaseIsReference(phase)) {
      total += 1;
      continue;
    }
    const ContentClass cls = phaseClass(phase);
    if (skipMask_ & classBit(cls)) continue;
    total += series(phasePolarity(phase), cls).rungs ? 1 : 3;
  }
  return static_cast<uint8_t>(total > 255 ? 255 : total);
}

size_t formatSessionRow(char* out, const size_t capacity, const SessionInfo& info) {
  Line line{out, capacity};
  const auto& c = info.config;
  line.add("{\"event\":\"session\",\"schema\":%d,\"session\":", kLogSchema);
  line.addString(info.session);
  line.add(",\"firmware\":");
  line.addString(info.firmware);
  line.add(",\"policy\":\"%s\",\"policy_version\":%u,\"policy_source\":", AdaptiveRefreshPolicy::kName,
           static_cast<unsigned>(AdaptiveRefreshPolicy::kVersion));
  line.addString(info.policySource);
  line.add(",\"policy_config\":{\"clear_threshold_permille\":%lu,\"night_clear_threshold_permille\":%lu,"
           "\"safety_cap\":%u,\"weight_grayscale_permille\":%lu,\"weight_image_permille\":%lu,"
           "\"weight_grayscale_image_extra_permille\":%lu,\"frame_charge_permille\":%lu,"
           "\"clear_after_image\":%s,\"image_waveform\":\"%s\"}",
           static_cast<unsigned long>(c.clearThresholdPermille), static_cast<unsigned long>(c.nightClearThresholdPermille),
           static_cast<unsigned>(c.safetyCap), static_cast<unsigned long>(c.weightGrayscalePermille),
           static_cast<unsigned long>(c.weightImagePermille),
           static_cast<unsigned long>(c.weightGrayscaleImageExtraPermille),
           static_cast<unsigned long>(c.frameChargePermille), c.clearAfterImage ? "true" : "false",
           refresh_policy_config::imageWaveformName(c.imageWaveform));
  line.add(",\"panel_pixels\":%lu,\"font\":", static_cast<unsigned long>(info.pixelCount));
  line.addString(info.fontName);
  line.add(",\"font_id\":%lu,\"point_size\":%lu,\"antialias\":%u,\"skip\":[",
           static_cast<unsigned long>(info.fontId), static_cast<unsigned long>(info.pointSize),
           info.antialias ? 1u : 0u);
  bool first = true;
  for (uint8_t i = 0; i < kClassCount; ++i) {
    if (!(info.skipMask & (1u << i))) continue;
    line.add("%s\"%s\"", first ? "" : ",", kClassNames[i]);
    first = false;
  }
  line.add("],\"fast_counts\":[");
  for (int8_t i = 0; i < kLevels; ++i) line.add("%s%u", i ? "," : "", static_cast<unsigned>(kFastCounts[i]));
  line.add("]}");
  return line.finish();
}

size_t formatRungRow(char* out, const size_t capacity, const Rung& rung, const RungMeasurement& m,
                     const uint8_t rating, const uint32_t elapsedMs) {
  Line line{out, capacity};
  char id[24];
  rung.formatId(id, sizeof(id));
  const bool reference = rung.kind == RungKind::Reference;
  line.add("{\"event\":\"rung\",\"seq\":%u,\"rung\":\"%s\",\"kind\":\"%s\",\"polarity\":\"%s\",\"class\":",
           static_cast<unsigned>(rung.seq), id, reference ? "reference" : "probe", polarityName(rung.polarity));
  if (reference) {
    line.add("null");
  } else {
    line.add("\"%s\"", className(rung.cls));
  }
  const unsigned long long debtPermille =
      m.pixelCount ? static_cast<unsigned long long>(m.weightedDebt) * 1000ull / m.pixelCount : 0ull;
  line.add(",\"level\":%d,\"fast_count\":%u,\"rating\":%u,\"frames\":%u,\"panel_pixels\":%lu,\"changed_px\":%llu,"
           "\"weighted_debt\":%lu,\"debt_permille\":%llu,\"policy_clear_at\":%u,\"ink_px_mean\":%lu,"
           "\"grayscale\":%u,\"image\":%u,\"elapsed_ms\":%lu}",
           static_cast<int>(rung.level), static_cast<unsigned>(rung.fastCount()), static_cast<unsigned>(rating),
           static_cast<unsigned>(m.frames), static_cast<unsigned long>(m.pixelCount),
           static_cast<unsigned long long>(m.changedPixels), static_cast<unsigned long>(m.weightedDebt), debtPermille,
           static_cast<unsigned>(m.policyClearAt), static_cast<unsigned long>(m.meanInkPixels),
           m.grayscale ? 1u : 0u, m.image ? 1u : 0u, static_cast<unsigned long>(elapsedMs));
  return line.finish();
}

size_t formatCompleteRow(char* out, const size_t capacity, const Ladder& ladder) {
  Line line{out, capacity};
  line.add("{\"event\":\"complete\",\"rated\":%u,\"series\":[", static_cast<unsigned>(ladder.rated()));
  bool first = true;
  for (uint8_t p = 0; p < 2; ++p) {
    for (uint8_t c = 0; c < kClassCount; ++c) {
      const auto polarity = static_cast<Polarity>(p);
      const auto cls = static_cast<ContentClass>(c);
      const Series& s = ladder.series(polarity, cls);
      line.add("%s{\"polarity\":\"%s\",\"class\":\"%s\",\"skipped\":%s,\"lo\":%d,\"hi\":%d,\"rungs\":%u}",
               first ? "" : ",", polarityName(polarity), className(cls),
               (ladder.skipMask() & classBit(cls)) ? "true" : "false", static_cast<int>(s.lo),
               static_cast<int>(s.hi), static_cast<unsigned>(s.rungs));
      first = false;
    }
  }
  line.add("]}");
  return line.finish();
}

Replay::Status Replay::feed(const char* text, const size_t length) {
  size_t end = length;
  while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r' || text[end - 1] == ' ')) --end;
  if (end == 0) return Status::Ok;
  JsonDocument doc;
  if (deserializeJson(doc, text, end) || !doc.is<JsonObjectConst>()) return started_ ? Status::BadRow : Status::BadSession;
  const char* event = doc["event"].as<const char*>();
  if (!event) return started_ ? Status::BadRow : Status::BadSession;

  if (!started_) {
    if (strcmp(event, "session") != 0) return Status::NeedSession;
    if (!doc["schema"].is<int>() || doc["schema"].as<int>() != kLogSchema) return Status::BadSession;
    const JsonArrayConst skip = doc["skip"].as<JsonArrayConst>();
    if (skip.isNull()) return Status::BadSession;
    uint8_t mask = 0;
    for (const JsonVariantConst name : skip) {
      ContentClass cls;
      if (!parseClass(name.as<const char*>(), cls)) return Status::BadSession;
      mask |= classBit(cls);
    }
    ladder_ = Ladder(mask);
    started_ = true;
    return Status::Ok;
  }

  if (strcmp(event, "rung") != 0) return strcmp(event, "session") == 0 ? Status::BadRow : Status::Ok;
  Rung rung;
  const char* kind = doc["kind"].as<const char*>();
  if (!kind || !doc["seq"].is<int>() || !doc["level"].is<int>() || !doc["rating"].is<int>()) return Status::BadRow;
  if (!parsePolarity(doc["polarity"].as<const char*>(), rung.polarity)) return Status::BadRow;
  if (strcmp(kind, "reference") == 0) {
    rung.kind = RungKind::Reference;
  } else if (strcmp(kind, "probe") == 0) {
    rung.kind = RungKind::Probe;
    if (!parseClass(doc["class"].as<const char*>(), rung.cls)) return Status::BadRow;
  } else {
    return Status::BadRow;
  }
  const int seq = doc["seq"].as<int>();
  const int level = doc["level"].as<int>();
  const int rating = doc["rating"].as<int>();
  if (seq < 0 || seq > 255 || level < -1 || level >= kLevels || rating < 0 || rating > kRatingBothers) {
    return Status::BadRow;
  }
  rung.seq = static_cast<uint8_t>(seq);
  rung.level = static_cast<int8_t>(level);
  return ladder_.rate(rung, static_cast<uint8_t>(rating)) ? Status::Ok : Status::Mismatch;
}

}  // namespace refresh_calibration
