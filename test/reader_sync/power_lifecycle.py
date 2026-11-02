#!/usr/bin/env python3
"""Compile the unchanged production power HAL against host hardware adapters.

Only GPIO/clock/Wi-Fi and FreeRTOS semaphore boundaries are adapted. Both HAL
source files are copied byte-for-byte into a temporary include directory so
quoted HAL dependencies resolve to adapters rather than requiring an ESP SDK.
No extracted/reimplemented lock algorithm, device, SD card or network is used.
Run: python3 test/reader_sync/power_lifecycle.py
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
ADAPTER = r"""#pragma once
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <mutex>

namespace hostPower {
inline std::atomic<int> cpuMHz{240};
inline std::atomic<int> wifiMode{0};
inline std::atomic<bool> failNextClockWrite{false};
inline std::atomic<unsigned> errors{0};
inline std::mutex modeMutex;
}
using SemaphoreHandle_t = std::mutex*;
constexpr unsigned portMAX_DELAY = 0;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return &hostPower::modeMutex; }
inline void xSemaphoreTake(SemaphoreHandle_t m, unsigned) { m->lock(); }
inline void xSemaphoreGive(SemaphoreHandle_t m) { m->unlock(); }
inline int getCpuFrequencyMhz() { return hostPower::cpuMHz.load(); }
inline bool setCpuFrequencyMhz(int mhz) {
  if (hostPower::failNextClockWrite.exchange(false)) return false;
  hostPower::cpuMHz = mhz;
  return true;
}
inline unsigned long millis() { return 1; }
constexpr int INPUT = 0, OUTPUT = 1, HIGH = 1;
inline void pinMode(int, int) { std::abort(); }
inline void digitalWrite(int, int) { std::abort(); }
using gpio_num_t = int;
constexpr gpio_num_t GPIO_NUM_13 = 13;
inline void gpio_hold_dis(gpio_num_t) { std::abort(); }
inline void gpio_hold_en(gpio_num_t) { std::abort(); }
#define SOC_PM_SUPPORT_EXT1_WAKEUP 1
#define FREEINK_DEVICE_PAPERMONO 0
#define LOG_DBG(...) do {} while (false)
#define LOG_ERR(...) do { ++hostPower::errors; } while (false)
constexpr int WIFI_MODE_NULL = 0;
struct HostWifi { int getMode() const { return hostPower::wifiMode.load(); } };
inline HostWifi WiFi;
class BatteryMonitor {
 public:
  bool readPercentageChecked(uint16_t& value) const { value = 63; return true; }
  uint16_t readPercentage() const { return 63; }
};
class HalGPIO {};
namespace BoardConfig {
struct Board {
  int batteryAdc = -1;
  struct { int gaugeAddr = 0; } batteryGauge;
  struct { int8_t latch0 = -1, latch1 = -1; } power;
};
inline constexpr Board ACTIVE{};
}
namespace freeink {
struct PowerManager {
  static void powerDownRailsForSleep() { std::abort(); }
  static void deepSleepUntilPowerButton() { std::abort(); }
};
}
"""

with tempfile.TemporaryDirectory(prefix="reader-power-") as scratch:
    work = Path(scratch)
    for name in ("HalPowerManager.cpp", "HalPowerManager.h"):
        shutil.copyfile(ROOT / "lib/hal" / name, work / name)
    (work / "hardware_adapters.h").write_text(ADAPTER)
    for name in ("Arduino.h", "BatteryMonitor.h", "InputManager.h", "Logging.h", "BoardConfig.h",
                 "PowerManager.h", "WiFi.h", "HalGPIO.h", "esp_sleep.h", "soc/soc_caps.h", "freertos/semphr.h"):
        target = work / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text('#pragma once\n#include "hardware_adapters.h"\n')
    for psram in (0, 1):
        binary = work / f"power-lifecycle-{psram}"
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-pthread", "-Wall", "-Wextra",
                        f"-DBOARD_HAS_PSRAM={psram}", "-I", str(work), str(work / "HalPowerManager.cpp"),
                        str(HERE / "power_lifecycle.cpp"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
