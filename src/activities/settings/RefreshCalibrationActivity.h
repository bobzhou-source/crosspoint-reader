#pragma once

#include <AdaptiveRefreshPolicy.h>
#include <RefreshCalibrationLadder.h>

#include <I18n.h>


#include "activities/Activity.h"

// Settings -> System -> Refresh calibration. Walks the refresh_calibration
// ladder: per rung a clearing refresh, N fast refreshes of a bundled fixture
// page, then a probe screen (white / gray / black fields, rung ID, fiducials)
// that Robert photographs and rates. Ratings append to
// /.crosspoint/refresh-calibration/<session>.jsonl; an interrupted run resumes
// from that log. Never touches the user's books or settings.
class RefreshCalibrationActivity final : public Activity {
 public:
  RefreshCalibrationActivity(GfxRenderer& renderer, MappedInputManager& input);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return stage == Stage::Running; }
  bool forcedInversion(bool& inverted) const override;

 private:
  enum class Stage : uint8_t { Intro, Running, Rating, Done, Error };
  enum class TextKind : uint8_t { Latin, Cjk };

  // Caller holds RenderLock.
  void inspectSession();
  bool startSession();
  void startRung();
  bool appendRow(const char* row, size_t length);
  void rate(uint8_t rating);
  void fail(StrId message);

  void renderMenu();
  void renderFrame();
  void drawProbe();
  void drawContent(int plane);
  void drawTextPage(TextKind kind, uint32_t offset, int top, int bottom, int maxWidth, int maxLines, bool record);
  void drawImage(int top, int bottom, uint8_t pass);
  void drawMenuPage(uint16_t selection);
  // 0/1/2 from a side key, a front button, or a tap on the bottom labels.
  // A tap above that band is a body tap (starts the run). -1 if nothing arrived.
  int readRating() const;
  void displayContentFrame(bool clear);
  void account(const AdaptiveRefreshPolicy::Inputs& inputs, bool clear);
  void commitPageAdvance();
  refresh_calibration::Ladder ladder;
  refresh_calibration::Rung rung;
  refresh_calibration::RungMeasurement measurement;
  unsigned long rungStartedMs = 0;
  AdaptiveRefreshPolicy driver;    // runs the hardware: fast unless a clear is requested
  AdaptiveRefreshPolicy observer;  // the active config, only watched
  AdaptiveRefreshPolicy meter;     // zero weights: unweighted change sum
  Stage stage = Stage::Intro;
  StrId message = StrId::STR_REFRESH_CALIBRATION_HELP;
  std::atomic<bool> frameDone{false};
  uint16_t frame = 0;
  uint64_t inkSum = 0;
  uint32_t textOffset[2] = {0, 0};
  uint32_t pageOffset = 0;  // start of the text page being drawn (AA passes redraw it)
  uint32_t pageEnd = 0;
  uint32_t imageSeed = 0;
  bool layoutRecorded = false;
  int fontId = 0;
  uint8_t skipMask = 0;
  uint8_t resumeRated = 0;
  bool resumable = false;
  bool damagedLog = false;
  char sessionId[16] = {};
  char logPath[80] = {};
  char line[192] = {};
  char row[refresh_calibration::kMaxRowBytes] = {};
};
