#pragma once

namespace night_mode {

// Reader overlays share the page's logical framebuffer and therefore its polarity.
constexpr bool invertActivity(bool night, bool darkMenus, bool reader) { return night && (reader || darkMenus); }

constexpr bool invertBuiltinSleep(bool night, bool lightSleepSelected) { return night || !lightSleepSelected; }

}  // namespace night_mode
