#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "AdaptiveRefreshPolicy.h"
#include "RefreshPolicyConfig.h"

namespace {

using refresh_policy_config::Config;
using refresh_policy_config::parse;
using refresh_policy_config::Source;
using refresh_policy_config::State;

State parseText(const std::string& text) { return parse(text.data(), text.size()); }

void expectRejected(const std::string& text, const char* reasonFragment) {
  const State state = parseText(text);
  EXPECT_EQ(state.source, Source::FileInvalid) << text;
  EXPECT_EQ(state.config, Config{}) << "a rejected file must leave every compiled default in force: " << text;
  EXPECT_NE(std::strstr(state.detail, reasonFragment), nullptr) << state.detail;
}

constexpr Config kCompiled{};

}  // namespace

TEST(RefreshPolicyConfig, CompiledDefaultsAreThePublishedPolicyConstants) {
  EXPECT_STREQ(AdaptiveRefreshPolicy::kName, "adaptive-content-v2");
  EXPECT_EQ(AdaptiveRefreshPolicy::kVersion, 2);
  EXPECT_EQ(kCompiled.clearThresholdPermille, 1500u);
  EXPECT_EQ(kCompiled.nightClearThresholdPermille, 1500u);
  EXPECT_EQ(kCompiled.safetyCap, 60u);
  EXPECT_EQ(kCompiled.weightGrayscalePermille, 250u);
  EXPECT_EQ(kCompiled.weightImagePermille, 1000u);
  EXPECT_EQ(kCompiled.weightGrayscaleImageExtraPermille, 500u);
  EXPECT_EQ(kCompiled.frameChargePermille, 0u);
}

TEST(RefreshPolicyConfig, FullFileIsAppliedVerbatim) {
  const State state = parseText(R"({"schema":1,"policy":"adaptive-content-v2",
    "clear_threshold_permille":2400,"night_clear_threshold_permille":3100,"safety_cap":90,
    "weight_grayscale_permille":100,"weight_image_permille":2000,
    "weight_grayscale_image_extra_permille":700,"frame_charge_permille":15,
    "meta":{"generated_by":"analyze.py","series":[1,2,3]}})");
  EXPECT_EQ(state.source, Source::File);
  EXPECT_EQ(state.clampedFields, 0u);
  EXPECT_EQ(state.config.clearThresholdPermille, 2400u);
  EXPECT_EQ(state.config.nightClearThresholdPermille, 3100u);
  EXPECT_EQ(state.config.safetyCap, 90u);
  EXPECT_EQ(state.config.weightGrayscalePermille, 100u);
  EXPECT_EQ(state.config.weightImagePermille, 2000u);
  EXPECT_EQ(state.config.weightGrayscaleImageExtraPermille, 700u);
  EXPECT_EQ(state.config.frameChargePermille, 15u);
}

TEST(RefreshPolicyConfig, MissingTunablesKeepDefaultsAndNightFollowsDay) {
  const State onlySchema = parseText(R"({"schema":1})");
  EXPECT_EQ(onlySchema.source, Source::File);
  EXPECT_EQ(onlySchema.config, kCompiled);

  const State dayOnly = parseText(R"({"schema":1,"clear_threshold_permille":2600})");
  EXPECT_EQ(dayOnly.source, Source::File);
  EXPECT_EQ(dayOnly.config.clearThresholdPermille, 2600u);
  EXPECT_EQ(dayOnly.config.nightClearThresholdPermille, 2600u);
  EXPECT_EQ(dayOnly.config.safetyCap, kCompiled.safetyCap);
  EXPECT_EQ(dayOnly.config.weightImagePermille, kCompiled.weightImagePermille);

  const State nightOnly = parseText(R"({"schema":1,"night_clear_threshold_permille":4000})");
  EXPECT_EQ(nightOnly.config.clearThresholdPermille, kCompiled.clearThresholdPermille);
  EXPECT_EQ(nightOnly.config.nightClearThresholdPermille, 4000u);
}

TEST(RefreshPolicyConfig, OutOfRangeValuesAreClampedAndReported) {
  const State state = parseText(
      R"({"schema":1,"clear_threshold_permille":5,"safety_cap":100000,"weight_image_permille":-3,"frame_charge_permille":1000})");
  EXPECT_EQ(state.source, Source::FileClamped);
  EXPECT_EQ(state.clampedFields, 3u);  // frame charge 1000 is the inclusive maximum, not clamped
  EXPECT_EQ(state.config.clearThresholdPermille, refresh_policy_config::kThresholdRange.min);
  EXPECT_EQ(state.config.nightClearThresholdPermille, refresh_policy_config::kThresholdRange.min);
  EXPECT_EQ(state.config.safetyCap, refresh_policy_config::kSafetyCapRange.max);
  EXPECT_EQ(state.config.weightImagePermille, 0u);
  EXPECT_EQ(state.config.frameChargePermille, 1000u);
  EXPECT_NE(std::strstr(state.detail, "clear_threshold_permille"), nullptr) << state.detail;

  const State huge = parseText(R"({"schema":1,"clear_threshold_permille":9223372036854775807})");
  EXPECT_EQ(huge.source, Source::FileClamped);
  EXPECT_EQ(huge.config.clearThresholdPermille, refresh_policy_config::kThresholdRange.max);
}

TEST(RefreshPolicyConfig, AnythingAmbiguousRejectsTheWholeFile) {
  expectRejected("", "empty");
  expectRejected("{\"schema\":1,", "malformed");
  expectRejected("[1,2,3]", "not a JSON object");
  expectRejected(R"({"clear_threshold_permille":2000})", "missing schema");
  expectRejected(R"({"schema":2,"clear_threshold_permille":2000})", "unsupported schema");
  expectRejected(R"({"schema":"1"})", "unsupported schema");
  expectRejected(R"({"schema":1,"policy":"adaptive-content-v1","clear_threshold_permille":2000})", "policy name");
  expectRejected(R"({"schema":1,"policy":2})", "policy name");
  // A typo must not silently leave the intended value at its default.
  expectRejected(R"({"schema":1,"clear_treshold_permille":2000})", "unknown key: clear_treshold_permille");
  expectRejected(R"({"schema":1,"clear_threshold_permille":2000.5})", "not an integer: clear_threshold_permille");
  expectRejected(R"({"schema":1,"safety_cap":"60"})", "not an integer: safety_cap");
  expectRejected(R"({"schema":1,"safety_cap":true})", "not an integer: safety_cap");
  expectRejected(R"({"schema":1,"safety_cap":null})", "not an integer: safety_cap");
  expectRejected(R"({"schema":1,"meta":"calibrated"})", "meta");
  // Values that were valid must not survive a later rejection.
  expectRejected(R"({"schema":1,"clear_threshold_permille":3000,"bogus":1})", "unknown key: bogus");
}

TEST(RefreshPolicyConfig, OversizedFilesAreRejectedBeforeParsing) {
  std::string big = R"({"schema":1,"meta":{"pad":")";
  big.append(refresh_policy_config::kMaxFileBytes, 'x');
  big += "\"}}";
  expectRejected(big, "too large");
  EXPECT_EQ(parse(nullptr, 10).source, Source::FileInvalid);
}

TEST(RefreshPolicyConfig, ActiveStateStartsCompiledAndIsReplaceable) {
  EXPECT_EQ(refresh_policy_config::active().source, Source::Compiled);
  EXPECT_EQ(refresh_policy_config::active().config, kCompiled);
  const State loaded = parseText(R"({"schema":1,"clear_threshold_permille":2222})");
  refresh_policy_config::setActive(loaded);
  EXPECT_EQ(refresh_policy_config::active().config.clearThresholdPermille, 2222u);
  EXPECT_TRUE(refresh_policy_config::fromFile(refresh_policy_config::active().source));
  refresh_policy_config::setActive(State{});
  EXPECT_EQ(refresh_policy_config::active().config, kCompiled);
  EXPECT_STREQ(refresh_policy_config::sourceName(Source::FileInvalid), "file-invalid");
}
