#pragma once
#include <ctime>

#include "Arduino.h"
struct HalClock {
  bool localTime(tm& out) const {
    const time_t epoch = 1793491200 + testMillis / 1000;
    gmtime_r(&epoch, &out);
    return true;
  }
};
inline HalClock halClock;
