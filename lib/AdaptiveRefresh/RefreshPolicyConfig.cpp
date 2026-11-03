#include "RefreshPolicyConfig.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

namespace refresh_policy_config {
namespace {

State activeState;

enum class Field : uint8_t {
  Threshold,
  NightThreshold,
  SafetyCap,
  WeightGrayscale,
  WeightImage,
  WeightGrayscaleImageExtra,
  FrameCharge,
};

struct Tunable {
  const char* key;
  Field field;
  Range range;
};

constexpr Tunable kTunables[] = {
    {"clear_threshold_permille", Field::Threshold, kThresholdRange},
    {"night_clear_threshold_permille", Field::NightThreshold, kThresholdRange},
    {"safety_cap", Field::SafetyCap, kSafetyCapRange},
    {"weight_grayscale_permille", Field::WeightGrayscale, kWeightRange},
    {"weight_image_permille", Field::WeightImage, kWeightRange},
    {"weight_grayscale_image_extra_permille", Field::WeightGrayscaleImageExtra, kWeightRange},
    {"frame_charge_permille", Field::FrameCharge, kFrameChargeRange},
};

void assign(Config& config, const Field field, const uint32_t value) {
  switch (field) {
    case Field::Threshold: config.clearThresholdPermille = value; break;
    case Field::NightThreshold: config.nightClearThresholdPermille = value; break;
    case Field::SafetyCap: config.safetyCap = static_cast<uint16_t>(value); break;
    case Field::WeightGrayscale: config.weightGrayscalePermille = value; break;
    case Field::WeightImage: config.weightImagePermille = value; break;
    case Field::WeightGrayscaleImageExtra: config.weightGrayscaleImageExtraPermille = value; break;
    case Field::FrameCharge: config.frameChargePermille = value; break;
  }
}

State invalid(const char* reason, const char* key = nullptr) {
  State state;
  state.source = Source::FileInvalid;
  if (key) {
    snprintf(state.detail, sizeof(state.detail), "%s: %s", reason, key);
  } else {
    snprintf(state.detail, sizeof(state.detail), "%s", reason);
  }
  return state;
}

}  // namespace

State parse(const char* data, const size_t length) {
  if (data == nullptr || length == 0) return invalid("empty file");
  if (length > kMaxFileBytes) return invalid("file too large");

  JsonDocument doc;
  if (deserializeJson(doc, data, length)) return invalid("malformed JSON");
  if (!doc.is<JsonObjectConst>()) return invalid("not a JSON object");
  const JsonObjectConst root = doc.as<JsonObjectConst>();

  const JsonVariantConst schema = root["schema"];
  if (schema.isNull()) return invalid("missing schema");
  if (!schema.is<long long>() || schema.as<long long>() != kSchema) return invalid("unsupported schema");
  const JsonVariantConst policy = root["policy"];
  if (!policy.isNull() && (!policy.is<const char*>() || strcmp(policy.as<const char*>(), AdaptiveRefreshPolicy::kName) != 0)) {
    return invalid("policy name mismatch");
  }

  State state;
  state.source = Source::File;
  bool nightGiven = false;
  for (const JsonPairConst pair : root) {
    const char* key = pair.key().c_str();
    if (strcmp(key, "schema") == 0 || strcmp(key, "policy") == 0) continue;
    if (strcmp(key, "meta") == 0) {
      if (!pair.value().is<JsonObjectConst>()) return invalid("meta is not an object");
      continue;
    }
    bool* const flag = strcmp(key, "clear_after_image") == 0    ? &state.config.clearAfterImage
                       : strcmp(key, "image_clear_region") == 0 ? &state.config.imageClearRegion
                                                                 : nullptr;
    if (flag) {
      if (!pair.value().is<bool>()) return invalid("not a bool", key);
      *flag = pair.value().as<bool>();
      continue;
    }
    if (strcmp(key, "image_waveform") == 0) {
      const char* name = pair.value().is<const char*>() ? pair.value().as<const char*>() : nullptr;
      if (name && strcmp(name, imageWaveformName(AdaptiveRefreshPolicy::ImageWaveform::Quality)) == 0) {
        state.config.imageWaveform = AdaptiveRefreshPolicy::ImageWaveform::Quality;
      } else if (name && strcmp(name, imageWaveformName(AdaptiveRefreshPolicy::ImageWaveform::Fast)) == 0) {
        state.config.imageWaveform = AdaptiveRefreshPolicy::ImageWaveform::Fast;
      } else {
        return invalid("not quality/fast", key);
      }
      continue;
    }
    const Tunable* tunable = nullptr;
    for (const auto& candidate : kTunables) {
      if (strcmp(candidate.key, key) == 0) {
        tunable = &candidate;
        break;
      }
    }
    if (!tunable) return invalid("unknown key", key);
    if (!pair.value().is<long long>()) return invalid("not an integer", key);
    const long long raw = pair.value().as<long long>();
    uint32_t value;
    if (raw < static_cast<long long>(tunable->range.min)) {
      value = tunable->range.min;
    } else if (raw > static_cast<long long>(tunable->range.max)) {
      value = tunable->range.max;
    } else {
      value = static_cast<uint32_t>(raw);
    }
    if (static_cast<long long>(value) != raw) {
      if (state.clampedFields == 0) snprintf(state.detail, sizeof(state.detail), "clamped: %s", key);
      if (state.clampedFields != 0xFF) ++state.clampedFields;
      state.source = Source::FileClamped;
    }
    assign(state.config, tunable->field, value);
    if (tunable->field == Field::NightThreshold) nightGiven = true;
  }
  if (!nightGiven) state.config.nightClearThresholdPermille = state.config.clearThresholdPermille;
  return state;
}

const char* sourceName(const Source source) {
  switch (source) {
    case Source::Compiled: return "compiled";
    case Source::File: return "file";
    case Source::FileClamped: return "file-clamped";
    case Source::FileInvalid: return "file-invalid";
    case Source::FileUnreadable: return "file-unreadable";
  }
  return "unknown";
}

const State& active() { return activeState; }

void setActive(const State& state) { activeState = state; }

}  // namespace refresh_policy_config
