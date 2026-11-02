#include "BenchmarkSettingsActivity.h"

#include <Benchmark.h>
#include <BenchmarkFixtures.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Memory.h>
#include <Txt.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "components/UITheme.h"
#include "network/ReaderSync.h"

namespace fui = freeink::ui;
namespace {
constexpr uint8_t TARGETS[] = {10, 3, 5, 1};
constexpr StrId LABELS[] = {StrId::STR_BENCHMARK_RUN, StrId::STR_BENCHMARK_RECORD,
                          StrId::STR_BENCHMARK_STATUS, StrId::STR_BENCHMARK_RESULT,
                          StrId::STR_BENCHMARK_COUNTS, StrId::STR_BENCHMARK_HELP};

void captureEnvironment() {
  benchmark::hardware();
  benchmark::settings(SETTINGS.fontFamily, SETTINGS.fontPointSize, SETTINGS.lineSpacing, SETTINGS.orientation,
                      SETTINGS.textAntiAliasing, SETTINGS.screenInverted, SETTINGS.screenMargin, SETTINGS.sleepScreen,
                      benchmark::id(SETTINGS.sdFontFamilyName));
}

StrId outcomeLabel(const BenchmarkOutcome outcome) {
  switch (outcome) {
    case BenchmarkOutcome::Complete: return StrId::STR_BENCHMARK_COMPLETE;
    case BenchmarkOutcome::Cancelled: return StrId::STR_BENCHMARK_CANCELLED;
    case BenchmarkOutcome::LoadFailed: return StrId::STR_BENCHMARK_LOAD_FAILED;
    case BenchmarkOutcome::RenderFailed: return StrId::STR_BENCHMARK_RENDER_FAILED;
    case BenchmarkOutcome::NoProgress: return StrId::STR_BENCHMARK_NO_PROGRESS;
    case BenchmarkOutcome::Timeout: return StrId::STR_BENCHMARK_TIMEOUT;
    case BenchmarkOutcome::RecorderFailed: return StrId::STR_BENCHMARK_RECORDER_FAILED;
  }
  return StrId::STR_BENCHMARK_RECORDER_FAILED;
}
}  // namespace

BenchmarkSettingsActivity::BenchmarkSettingsActivity(GfxRenderer& renderer, MappedInputManager& input)
    : UiListActivity("BenchmarkSettings", renderer, input) {}

const char* BenchmarkSettingsActivity::headerTitle() const { return tr(STR_BENCHMARK); }

void BenchmarkSettingsActivity::onEnter() {
  for (int i = 0; i < ITEM_COUNT; ++i) {
    rows[i].label = I18N.get(LABELS[i]);
    rows[i].actionValue = i;
  }
  observedRecording = benchmark::enabled();
  UiListActivity::onEnter();
}

void BenchmarkSettingsActivity::onExit() {
  // Stack replacement (Home, sleep, USB) bypasses the pushed child's result handler.
  // ActivityManager already owns RenderLock and destroys children before parents.
  if (running) finishRun(BenchmarkOutcome::Cancelled);
  UiListActivity::onExit();
}

void BenchmarkSettingsActivity::loop() {
  UiListActivity::loop();
  const bool recording = benchmark::enabled();
  if (observedRecording != recording) {
    RenderLock lock;
    if (!recording) {
      if (statusLabel == StrId::STR_BENCHMARK_RECORDING) {
        statusLabel = StrId::STR_BENCHMARK_RECORDER_FAILED;
      } else if (SETTINGS.benchmarkRecord && resultPath[0]) {
        // Preserve the automatic result; its resumed normal session is separate.
        resumeFailed = true;
      }
    }
    observedRecording = recording;
    requestUpdate();
  }
}

void BenchmarkSettingsActivity::activateIndex(const int index) {
  nav.selected = index;
  app.clearTapFlash();
  if (running) return;
  if (index == 0) beginRun();
  else if (index == 1) toggleRecording();
  requestUpdate();
}

void BenchmarkSettingsActivity::toggleRecording() {
  RenderLock lock;
  const uint8_t previous = SETTINGS.benchmarkRecord;
  SETTINGS.benchmarkRecord = !previous;
  if (!SETTINGS.saveToFile()) {
    SETTINGS.benchmarkRecord = previous;
    statusLabel = StrId::STR_BENCHMARK_SETTING_FAILED;
    return;
  }
  resumeFailed = false;
  if (SETTINGS.benchmarkRecord) {
    if (!benchmark::begin(benchmark::SessionMode::Normal)) {
      statusLabel = StrId::STR_BENCHMARK_RECORDER_FAILED;
      return;
    }
    captureEnvironment();
    statusLabel = benchmark::enabled() ? StrId::STR_BENCHMARK_RECORDING : StrId::STR_BENCHMARK_RECORDER_FAILED;
  } else {
    const bool wasRecording = benchmark::enabled();
    const bool closed = wasRecording && benchmark::stop(benchmark::StopReason::User);
    statusLabel = wasRecording && !closed ? StrId::STR_BENCHMARK_RECORDER_FAILED : StrId::STR_BENCHMARK_STOPPED;
  }
}

void BenchmarkSettingsActivity::beginRun() {
  reader_sync::cancelAndJoin();
  {
    RenderLock lock;
    finalized = false;
    resumeFailed = false;
    resultPath[0] = '\0';
    phase = 0;
    for (auto& count : completed) count = 0;
    // Do not use getReaderFontId() for an SD selection: it silently falls back
    // to a built-in when loading failed. Benchmark exactly the resident selection.
    const int fontId = SETTINGS.sdFontFamilyName[0]
                           ? sdFontSystem.resolveFontId(SETTINGS.sdFontFamilyName, SETTINGS.fontPointSize)
                           : SETTINGS.getReaderFontId();
    if (fontId == 0 || renderer.getFontMap().find(fontId) == renderer.getFontMap().end()) {
      statusLabel = StrId::STR_BENCHMARK_FONT_UNAVAILABLE;
      return;
    }
    const auto prepared = benchmark_fixtures::prepare();
    if (prepared != benchmark_fixtures::PrepareResult::Ready) {
      statusLabel = prepared == benchmark_fixtures::PrepareResult::Conflict
                        ? StrId::STR_BENCHMARK_FIXTURE_CONFLICT : StrId::STR_BENCHMARK_FIXTURE_FAILED;
      return;
    }
    // Use the format libraries' canonical path hashing and deletion scope. The
    // EPUB object exceeds the small task-stack budget, so give it short heap ownership.
    auto epub = makeUniqueNoThrow<Epub>(benchmark_fixtures::epubPath(), "/.crosspoint");
    if (!epub) {
      LOG_ERR("BENCH", "OOM: fixture EPUB cache handle");
      statusLabel = StrId::STR_BENCHMARK_LOAD_FAILED;
      return;
    }
    if (!epub->clearCache()) {
      statusLabel = StrId::STR_BENCHMARK_CACHE_FAILED;
      return;
    }
    epub.reset();
    Txt txt(benchmark_fixtures::txtPath(), "/.crosspoint");
    if (!txt.clearCache()) {
      statusLabel = StrId::STR_BENCHMARK_CACHE_FAILED;
      return;
    }
    resumeNormal = SETTINGS.benchmarkRecord || benchmark::enabled();
    if (benchmark::enabled() && !benchmark::stop(benchmark::StopReason::User)) {
      statusLabel = StrId::STR_BENCHMARK_RECORDER_FAILED;
      resumeNormalRecording();
      return;
    }
    if (!benchmark::begin(benchmark::SessionMode::Automatic)) {
      statusLabel = StrId::STR_BENCHMARK_RECORDER_FAILED;
      resumeNormalRecording();
      return;
    }
    snprintf(resultPath, sizeof(resultPath), "%s", benchmark::sessionPath());
    captureEnvironment();
    running = true;
    phase = 1;
    runStarted = millis();
    statusLabel = StrId::STR_BENCHMARK_RUNNING;
  }
  startPhase();
}

void BenchmarkSettingsActivity::startPhase() {
  RenderLock lock;
  if (!running) return;
  benchmark::flush();
  if (!benchmark::enabled()) {
    finishRun(BenchmarkOutcome::RecorderFailed);
    return;
  }
  benchmark::point(benchmark::EventType::RunStep, phase, 0, TARGETS[phase - 1], 9);
  if (phase == 4) {
    auto viewer = makeUniqueNoThrow<BmpViewerActivity>(renderer, mappedInput, benchmark_fixtures::bitmapPath());
    if (!viewer) {
      LOG_ERR("BENCH", "OOM: fixture bitmap viewer");
      finishRun(BenchmarkOutcome::LoadFailed);
      return;
    }
    viewer->configureBenchmark(completed[3]);
    startActivityForResult(std::move(viewer), [this](const ActivityResult& result) { childFinished(result); });
  } else {
    const char* path = phase == 3 ? benchmark_fixtures::txtPath() : benchmark_fixtures::epubPath();
    auto reader = ReaderActivity::create(renderer, mappedInput, path, false);
    if (!reader) {
      finishRun(BenchmarkOutcome::LoadFailed);
      return;
    }
    reader->configureBenchmark(phase, TARGETS[phase - 1], completed[phase - 1]);
    startActivityForResult(std::move(reader), [this](const ActivityResult& result) { childFinished(result); });
  }
}

void BenchmarkSettingsActivity::childFinished(const ActivityResult& result) {
  {
    RenderLock lock;
    if (!running) return;
    const auto* child = std::get_if<BenchmarkResult>(&result.data);
    const auto outcome = child && !result.isCancelled ? child->outcome : BenchmarkOutcome::Cancelled;
    if (outcome != BenchmarkOutcome::Complete) {
      finishRun(outcome);
      return;
    }
    if (completed[phase - 1] != TARGETS[phase - 1]) {
      finishRun(BenchmarkOutcome::NoProgress);
      return;
    }
    if (phase == 4) {
      finishRun(BenchmarkOutcome::Complete);
      return;
    }
    ++phase;
  }
  startPhase();
}

void BenchmarkSettingsActivity::resumeNormalRecording() {
  if (!resumeNormal) return;
  resumeNormal = false;
  const bool started = benchmark::begin(benchmark::SessionMode::Normal);
  if (started) captureEnvironment();
  resumeFailed = !started || !benchmark::enabled();
}

void BenchmarkSettingsActivity::finishRun(const BenchmarkOutcome outcome) {
  if (!running) return;
  // Make room for the summary even after a verbose final grayscale render.
  benchmark::flush();
  benchmark::Event event{};
  event.type = benchmark::EventType::RunResult;
  event.startUs = event.endUs = benchmark::nowUs();
  event.data[0] = static_cast<uint32_t>(outcome);
  for (int i = 0; i < 4; ++i) event.data[i + 1] = completed[i];
  event.data[5] = phase;
  event.data[6] = millis() - runStarted;
  benchmark::record(event);
  const auto reason = outcome == BenchmarkOutcome::Complete ? benchmark::StopReason::Complete
                      : outcome == BenchmarkOutcome::Cancelled ? benchmark::StopReason::Cancelled
                                                               : benchmark::StopReason::Failure;
  finalized = benchmark::stop(reason);
  statusLabel = outcomeLabel(finalized ? outcome : BenchmarkOutcome::RecorderFailed);
  running = false;
  // resultPath was copied before this begin can replace the recorder's latest path.
  resumeNormalRecording();
}

void BenchmarkSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                     static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  rows[0].value = "";
  rows[1].value = SETTINGS.benchmarkRecord ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  rows[1].subtitle = resumeFailed ? tr(STR_BENCHMARK_RESUME_FAILED)
                       : benchmark::enabled() && !running ? tr(STR_BENCHMARK_RECORDING)
                       : SETTINGS.benchmarkRecord ? tr(STR_BENCHMARK_RECORDING_STOPPED) : "";
  rows[2].subtitle = I18N.get(statusLabel);
  rows[3].value = resultPath[0] ? (finalized ? tr(STR_BENCHMARK_SAVED) : tr(STR_BENCHMARK_PARTIAL)) : "";
  rows[3].subtitle = resultPath[0] ? resultPath : tr(STR_NOT_SET);
  snprintf(counts, sizeof(counts), tr(STR_BENCHMARK_COUNTS_FORMAT), completed[0], completed[1], completed[2], completed[3]);
  rows[4].subtitle = counts;
  rows[5].subtitle = tr(STR_BENCHMARK_HELP_DETAIL);
  fui::ListProps props;
  props.items = rows;
  props.count = ITEM_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 0;
  props.valueMaxWidthPercent = 40;
  props.labelMinWidthPercent = 55;
  props.valueText = screen.theme().smallText;
  props.valueText.maxLines = 2;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 4;
  syncListViewport(screen, props);
  screen.list(props);
}
