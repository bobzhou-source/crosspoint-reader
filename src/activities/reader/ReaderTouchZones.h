#pragma once

#include <cstdint>

// Reader touch zones and who owns a press and hold. Pure geometry, so the
// page-turn taps and the press-and-hold lookup share one definition.
namespace ReaderTouchZones {

enum class Zone : uint8_t { None, Menu, Prev, Next };

struct Layout {
  bool nextTaps;  // the page-turn gesture accepts taps
  bool prevTaps;  // the previous-page gesture accepts taps
  bool inverted;  // Inverted Tap or an RTL book reverses the shared zones
  bool menuTap;   // a tap in the centre third opens the reader menu
};

// The zone a tap at logical (x, y) lands in on a width x height page. The
// centre menu rectangle keeps priority over the page-turn zones. The sole
// tap-enabled direction gets the whole page; when both accept taps the page
// splits at the left third.
inline Zone zoneAt(const Layout& layout, const int x, const int y, const int width, const int height) {
  if (layout.menuTap && x >= width / 3 && x < width - width / 3 && y >= height / 3 && y < height - height / 3) {
    return Zone::Menu;
  }
  if (!layout.nextTaps && !layout.prevTaps) return Zone::None;
  const bool nextZone = layout.inverted ? x < (width * 2) / 3 : x >= width / 3;
  return layout.nextTaps && (!layout.prevTaps || nextZone) ? Zone::Next : Zone::Prev;
}

// A held page-turn tap already means something when a hold action (chapter
// skip, rotate) is set, so that zone keeps the hold.
inline bool zoneOwnsHold(const Zone zone, const bool zoneHoldActions) {
  return zoneHoldActions && (zone == Zone::Prev || zone == Zone::Next);
}

enum class HoldAction : uint8_t { None, Lookup, NoDictionary };

// What a press and hold in the reader does: nothing where the zone owns the
// hold or over blank space; on a word, a lookup, or the note that no
// dictionary is chosen.
inline HoldAction holdAction(const Zone zone, const bool zoneHoldActions, const bool wordHit,
                             const bool dictionarySet) {
  if (zoneOwnsHold(zone, zoneHoldActions) || !wordHit) return HoldAction::None;
  return dictionarySet ? HoldAction::Lookup : HoldAction::NoDictionary;
}

}  // namespace ReaderTouchZones
