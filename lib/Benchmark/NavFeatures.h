#pragma once

#include <cstddef>
#include <cstdint>

// The features a script step may name in `needs=`. A step whose feature is absent is reported as skipped (never
// as passed), so a card without the fixtures cannot read as full coverage.
//
// Presence is decided from the card and the firmware, never from the script: a fixture feature is true when its
// file exists, a firmware feature is true when the build carries the screen.
namespace benchmark::nav {

class FeatureProbe {
 public:
  virtual ~FeatureProbe() = default;
  virtual bool exists(const char* path) const = 0;
  virtual uint32_t recentCount() const = 0;
  virtual bool dictionarySelected() const = 0;  // Settings > Reader > Dictionary names a folder
  virtual bool touch() const = 0;
  virtual bool opdsConfigured() const = 0;  // Home carries an OPDS row between Files and File Transfer
  // No path to a network exists right now: no credential in the in-memory store, Wi-Fi off and not connected.
  virtual bool networkIdle() const = 0;
};

struct FeatureRule {
  const char* name;
  const char* path;   // fixture the feature needs on the card; null for state/firmware features
  const char* path2;  // a second fixture that must also exist (a book and its dictionary), or null
};

extern const FeatureRule FEATURE_RULES[];
extern const size_t FEATURE_RULE_COUNT;

// id is benchmark::id(name). Unknown ids are unavailable.
bool featureAvailable(const FeatureProbe& probe, uint32_t id);

}  // namespace benchmark::nav
