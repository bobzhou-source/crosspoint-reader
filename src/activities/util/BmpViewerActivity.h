#pragma once

#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void configureBenchmark(uint8_t& completed) { benchmarkCompleted = &completed; }
  bool isBenchmarkActivity() const override { return benchmarkCompleted != nullptr; }
  bool preventAutoSleep() override { return isBenchmarkActivity(); }

 private:
  void loadSiblingImages();
  void doSetSleepCover();
  bool canSetSleepCover() const;
  bool renderPng();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
  uint8_t* benchmarkCompleted = nullptr;
  uint32_t benchmarkPaintedAt = 0;
  bool painted = false;
};
