#pragma once

#include <I18n.h>

#include "activities/UiListActivity.h"

class BenchmarkSettingsActivity final : public UiListActivity {
 public:
  BenchmarkSettingsActivity(GfxRenderer& renderer, MappedInputManager& input);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return running; }
  bool isBenchmarkActivity() const override { return running; }

 private:
  static constexpr int ITEM_COUNT = 7;
  int listCount() const override { return ITEM_COUNT; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void beginRun();
  void beginNavRun();
  void buildNavRow();
  void startPhase();
  void childFinished(const ActivityResult& result);
  // Caller owns RenderLock, including the ActivityManager onExit path.
  void finishRun(BenchmarkOutcome outcome);
  void resumeNormalRecording();
  void toggleRecording();

  freeink::ui::ListItem rows[ITEM_COUNT]{};
  StrId statusLabel = StrId::STR_BENCHMARK_READY;
  char resultPath[80]{};
  char counts[64]{};
  char navText[256]{};
  StrId navStartLabel_ = StrId::STR_BENCHMARK_RECORDER_FAILED;
  bool navStartFailed = false;
  uint8_t completed[4]{};
  uint8_t phase = 0;
  uint32_t runStarted = 0;
  bool running = false;
  bool resumeNormal = false;
  bool resumeFailed = false;
  bool finalized = false;
  bool observedRecording = false;
};
