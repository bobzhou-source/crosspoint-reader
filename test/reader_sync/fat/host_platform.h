#pragma once
// Minimal host platform declarations for building the pinned SdFat FatLib
// without an Arduino runtime. SdFat is still compiled from its own sources;
// nothing here changes FAT or directory-entry behavior.
//
// Arduino.h declares this type. SdFat's non-Arduino PrintBasic still declares
// its print/println overloads, but only the AVR branch dereferences it, so a
// forward declaration is sufficient on the host.
class __FlashStringHelper;

// FatFileLFN.cpp calls millis() when it must generate a unique short name after
// an SFN collision. On Arduino this comes from Arduino.h, which the host build
// deliberately does not include.
#include <chrono>

inline unsigned long millis() {
  static const auto start = std::chrono::steady_clock::now();
  return static_cast<unsigned long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}
