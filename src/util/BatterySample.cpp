#include "BatterySample.h"

#include <BatteryMonitor.h>

namespace battery_sample {
Sample read() {
  static const BatteryMonitor battery;
  const auto status = battery.readStatus();
  Sample sample;
  if (!status.supported) return sample;
  if (status.millivoltsKnown) sample.millivolts = status.millivolts;
  if (status.percentageKnown) sample.percent = status.percentage;
  sample.charging = status.chargingKnown && status.charging;
  sample.valid = status.millivoltsKnown || status.percentageKnown;
  return sample;
}
}  // namespace battery_sample
