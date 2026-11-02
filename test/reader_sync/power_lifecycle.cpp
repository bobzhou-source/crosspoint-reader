#include "HalPowerManager.h"

#include <cassert>
#include <iostream>
#include <optional>
#include <semaphore>
#include <thread>

int main() {
  powerManager.begin();
  const auto idle = [] {
    powerManager.setPowerSaving(true);
    assert(hostPower::cpuMHz.load() == HalPowerManager::LOW_POWER_FREQ);
  };
  const auto held = [] {
    powerManager.setPowerSaving(true);
    assert(hostPower::cpuMHz.load() == 240);
  };
  idle();

  // A later renderer must keep its lease after the earlier worker finishes.
  std::optional<HalPowerManager::Lock> worker;
  std::optional<HalPowerManager::Lock> renderer;
  worker.emplace();
  renderer.emplace();
  held();
  worker.reset();
  held();
  renderer.reset();
  idle();

  // The reverse nesting order must also preserve the outer lease.
  {
    HalPowerManager::Lock outer;
    { HalPowerManager::Lock inner; held(); }
    held();
  }
  idle();

  // Exercise real overlapping threads, not just nested lexical scopes.
  std::binary_semaphore acquired{0}, finish{0}, released{0};
  std::thread background([&] {
    {
      HalPowerManager::Lock lease;
      acquired.release();
      finish.acquire();
    }
    released.release();
  });
  acquired.acquire();
  {
    HalPowerManager::Lock display;
    finish.release();
    released.acquire();
    held();
  }
  background.join();
  idle();

  // A failed hardware frequency change must neither strand the mutex nor
  // falsely publish the new mode and suppress the next retry.
  hostPower::failNextClockWrite = true;
  {
    HalPowerManager::Lock lease;
    assert(hostPower::cpuMHz.load() == HalPowerManager::LOW_POWER_FREQ);
    held();
  }
  hostPower::failNextClockWrite = true;
  powerManager.setPowerSaving(true);
  assert(hostPower::cpuMHz.load() == 240);
  idle();

  // Keep the preexisting independent Wi-Fi protection when no leases remain.
  hostPower::wifiMode = 1;
  held();
  hostPower::wifiMode = 0;
  idle();
  assert(hostPower::errors.load() == 0);
  std::cout << "PASS: production HalPowerManager overlapping leases, clock failure retries and Wi-Fi override (PSRAM="
            << BOARD_HAS_PSRAM << ")\n";
}
