#pragma once

#include <cstdint>

// One fresh battery reading for sync and session-log telemetry. Unlike
// HalPowerManager::getBatteryPercentage() this does not feed the UI's smoothed
// percentage, and it reports millivolts, which the smoothed value hides.
namespace battery_sample {
struct Sample {
  bool valid = false;        // at least one of mV/percent was readable
  bool charging = false;     // charger reported charging (false when unknown)
  uint16_t millivolts = 0;   // 0 when unknown
  uint16_t percent = 0;      // 0 when unknown; gauge SoC, or the voltage-curve estimate
};

// Call from the main task only: fuel-gauge boards read over the shared I2C bus.
Sample read();
}  // namespace battery_sample
