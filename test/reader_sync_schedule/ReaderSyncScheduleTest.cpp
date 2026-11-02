#include <gtest/gtest.h>

#include "ReaderSyncSchedule.h"

using namespace reader_sync;

namespace {
constexpr int64_t NOW = MIN_VALID_UTC + 100000;

// Home is on screen, the service is configured and enabled, boot just happened.
ScheduleState awakeHome() {
  ScheduleState s;
  s.configured = true;
  s.enabled = true;
  s.uiPermitted = true;
  s.homeShown = true;
  s.wakePending = true;
  s.nowMs = 100000;
  s.nextAttemptMs = 0;
  return s;
}

ScheduleState sleeping() {
  ScheduleState s;
  s.configured = true;
  s.enabled = true;
  s.sleeping = true;
  s.pendingUploads = true;
  s.nowMs = 100000;
  return s;
}

Trigger triggerOf(const ScheduleState& s) { return decide(s).trigger; }
}  // namespace

TEST(ReaderSyncScheduleParse, AcceptsOnlyTheTwoSpellings) {
  Schedule out = Schedule::Interval;
  EXPECT_TRUE(parseSchedule("events", out));
  EXPECT_EQ(out, Schedule::Events);
  EXPECT_TRUE(parseSchedule("interval", out));
  EXPECT_EQ(out, Schedule::Interval);
  out = Schedule::Events;
  EXPECT_FALSE(parseSchedule("Events", out));
  EXPECT_FALSE(parseSchedule("hourly", out));
  EXPECT_FALSE(parseSchedule("", out));
  EXPECT_FALSE(parseSchedule(nullptr, out));
  EXPECT_EQ(out, Schedule::Events) << "a rejected value must not modify the output";
  EXPECT_STREQ(scheduleName(Schedule::Events), "events");
  EXPECT_STREQ(scheduleName(Schedule::Interval), "interval");
}

TEST(ReaderSyncScheduleParse, DefaultStateIsEvents) {
  EXPECT_EQ(ScheduleState{}.schedule, Schedule::Events);
}

TEST(ReaderSyncScheduleParse, TriggerNumbersAreFrozenForTheSessionLog) {
  EXPECT_EQ(static_cast<int>(Trigger::None), 0);
  EXPECT_EQ(static_cast<int>(Trigger::Manual), 1);
  EXPECT_EQ(static_cast<int>(Trigger::Wake), 2);
  EXPECT_EQ(static_cast<int>(Trigger::PreSleep), 3);
  EXPECT_EQ(static_cast<int>(Trigger::Charging), 4);
  EXPECT_EQ(static_cast<int>(Trigger::Interval), 5);
  EXPECT_EQ(static_cast<int>(Trigger::Continue), 6);
  EXPECT_STREQ(triggerName(Trigger::Wake), "wake");
  EXPECT_STREQ(triggerName(Trigger::PreSleep), "pre_sleep");
  EXPECT_STREQ(triggerName(Trigger::None), "none");
}

TEST(ReaderSyncScheduleAge, UnknownWhenClockUnusable) {
  EXPECT_EQ(ageSeconds(NOW, NOW - 60), 60U);
  EXPECT_EQ(ageSeconds(NOW, NOW), 0U);
  EXPECT_EQ(ageSeconds(NOW, 0), AGE_UNKNOWN);
  EXPECT_EQ(ageSeconds(0, NOW), AGE_UNKNOWN);
  EXPECT_EQ(ageSeconds(NOW, NOW + 1), AGE_UNKNOWN) << "clock moved backwards";
  EXPECT_FALSE(within(AGE_UNKNOWN, 900));
  EXPECT_TRUE(within(899, 900));
  EXPECT_FALSE(within(900, 900));
}

TEST(ReaderSyncScheduleAge, DeadlineSurvivesMillisWraparound) {
  EXPECT_TRUE(deadlineReached(100, 100));
  EXPECT_FALSE(deadlineReached(99, 100));
  EXPECT_FALSE(deadlineReached(UINT32_MAX - 10, 5U)) << "deadline just past the wrap";
  EXPECT_TRUE(deadlineReached(10U, UINT32_MAX - 5U)) << "now just past the wrap";
}

TEST(ReaderSyncSchedulePreconditions, NeverStartsWhenBlocked) {
  auto s = awakeHome();
  ASSERT_EQ(triggerOf(s), Trigger::Wake);

  auto notConfigured = s;
  notConfigured.configured = false;
  notConfigured.manualRequested = true;
  EXPECT_EQ(triggerOf(notConfigured), Trigger::None);

  auto busy = s;
  busy.workerActive = true;
  EXPECT_EQ(triggerOf(busy), Trigger::None);

  auto radio = s;
  radio.radioForeground = true;
  radio.manualRequested = true;
  EXPECT_EQ(triggerOf(radio), Trigger::None) << "a foreground Wi-Fi feature owns the radio";

  auto parked = s;
  parked.parkedUntilWake = true;
  parked.manualRequested = true;
  EXPECT_EQ(triggerOf(parked), Trigger::None);

  auto hidden = s;
  hidden.uiPermitted = false;
  EXPECT_EQ(triggerOf(hidden), Trigger::None);

  auto disabled = s;
  disabled.enabled = false;
  EXPECT_EQ(triggerOf(disabled), Trigger::None);
}

TEST(ReaderSyncScheduleEvents, WakeWaitsForHome) {
  auto s = awakeHome();
  EXPECT_EQ(triggerOf(s), Trigger::Wake);
  s.homeShown = false;
  EXPECT_EQ(triggerOf(s), Trigger::None) << "waking straight into a book defers the sync";
  s.homeShown = true;
  EXPECT_EQ(triggerOf(s), Trigger::Wake) << "Home shown later in the session still fires it";
}

TEST(ReaderSyncScheduleEvents, NoSyncWhileBookIsOpening) {
  auto s = awakeHome();
  s.bookOpening = true;
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s.externalPower = true;
  s.continuation = true;
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s.bookOpening = false;
  EXPECT_NE(triggerOf(s), Trigger::None);
}

TEST(ReaderSyncScheduleEvents, WakeIsFiredOnceThenNotAgain) {
  auto s = awakeHome();
  EXPECT_EQ(triggerOf(s), Trigger::Wake);
  s.wakePending = false;  // the run cleared it
  s.nextAttemptMs = s.nowMs + 900000;
  EXPECT_EQ(triggerOf(s), Trigger::None);
}

TEST(ReaderSyncScheduleEvents, RecentSuccessDebouncesTheWakeAndConsumesIt) {
  auto s = awakeHome();
  s.lastSuccessAgeS = WAKE_DEBOUNCE_SECONDS - 1;
  auto d = decide(s);
  EXPECT_EQ(d.trigger, Trigger::None);
  EXPECT_TRUE(d.consumeWake) << "otherwise it would fire later in the same session";

  s.lastSuccessAgeS = WAKE_DEBOUNCE_SECONDS;
  d = decide(s);
  EXPECT_EQ(d.trigger, Trigger::Wake);
  EXPECT_FALSE(d.consumeWake);

  s.lastSuccessAgeS = AGE_UNKNOWN;
  EXPECT_EQ(triggerOf(s), Trigger::Wake) << "an unknown age never suppresses a sync";
}

TEST(ReaderSyncScheduleEvents, RecentFailedAttemptAlsoDebouncesTheWake) {
  auto s = awakeHome();
  s.lastAttemptAgeS = RETRY_DEBOUNCE_SECONDS - 1;
  auto d = decide(s);
  EXPECT_EQ(d.trigger, Trigger::None);
  EXPECT_TRUE(d.consumeWake);
  s.lastAttemptAgeS = RETRY_DEBOUNCE_SECONDS;
  EXPECT_EQ(triggerOf(s), Trigger::Wake);
}

TEST(ReaderSyncScheduleEvents, DebounceIsNotAffectedByALongerInterval) {
  auto s = awakeHome();
  s.intervalSeconds = 3600;
  s.lastSuccessAgeS = 20 * 60;  // older than the 15 minute debounce, younger than the interval
  EXPECT_EQ(triggerOf(s), Trigger::Wake);
}

TEST(ReaderSyncScheduleEvents, CancelledAttemptBacksOffBeforeTheWakeRetries) {
  auto s = awakeHome();
  s.nextAttemptMs = s.nowMs + 60000;
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s.nowMs += 60000;
  EXPECT_EQ(triggerOf(s), Trigger::Wake);
}

TEST(ReaderSyncScheduleEvents, BatteryOnlyNeverSyncsPeriodically) {
  auto s = awakeHome();
  s.wakePending = false;
  s.externalPower = false;
  s.nowMs = 10 * 60 * 60 * 1000;  // ten hours awake on battery
  EXPECT_EQ(triggerOf(s), Trigger::None);
}

TEST(ReaderSyncScheduleEvents, ChargingSyncsPeriodically) {
  auto s = awakeHome();
  s.wakePending = false;
  s.externalPower = true;
  s.intervalSeconds = 900;
  s.lastSuccessAgeS = 899;
  s.lastAttemptAgeS = 899;
  EXPECT_EQ(triggerOf(s), Trigger::None) << "interval not yet elapsed";
  s.lastSuccessAgeS = 900;
  s.lastAttemptAgeS = 900;
  EXPECT_EQ(triggerOf(s), Trigger::Charging);
  s.nextAttemptMs = s.nowMs + 1000;
  EXPECT_EQ(triggerOf(s), Trigger::None) << "the in-boot deadline also has to be reached";
  s.nowMs += 1000;
  EXPECT_EQ(triggerOf(s), Trigger::Charging);
}

TEST(ReaderSyncScheduleEvents, ChargingUsesTheNewestRunIncludingFailedAttempts) {
  auto s = awakeHome();
  s.wakePending = false;
  s.externalPower = true;
  s.lastSuccessAgeS = 5000;
  s.lastAttemptAgeS = 100;  // offline retry a moment ago
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s.lastAttemptAgeS = AGE_UNKNOWN;
  EXPECT_EQ(triggerOf(s), Trigger::Charging);
  s.lastSuccessAgeS = AGE_UNKNOWN;
  EXPECT_EQ(triggerOf(s), Trigger::Charging) << "never synced, charger present";
}

TEST(ReaderSyncScheduleEvents, DebouncedWakeOnChargerDoesNotRestartTheInterval) {
  auto s = awakeHome();
  s.externalPower = true;
  s.lastSuccessAgeS = 120;
  auto d = decide(s);
  EXPECT_TRUE(d.consumeWake);
  EXPECT_EQ(d.trigger, Trigger::None) << "a sync two minutes ago covers the wake and the charger";
}

TEST(ReaderSyncScheduleEvents, ChargingFiresAfterTheWakeWasDebounced) {
  auto s = awakeHome();
  s.externalPower = true;
  s.intervalSeconds = 900;
  s.lastSuccessAgeS = 14 * 60;  // inside the wake debounce but not yet a full interval
  s.lastAttemptAgeS = 14 * 60;
  auto d = decide(s);
  EXPECT_TRUE(d.consumeWake);
  EXPECT_EQ(d.trigger, Trigger::None);
  s.wakePending = false;
  s.lastSuccessAgeS = 15 * 60;
  s.lastAttemptAgeS = 15 * 60;
  EXPECT_EQ(triggerOf(s), Trigger::Charging);
}

TEST(ReaderSyncScheduleEvents, ContinuationFiresOnBatteryOnceDue) {
  auto s = awakeHome();
  s.wakePending = false;
  s.continuation = true;
  s.nextAttemptMs = s.nowMs + 60000;
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s.nowMs += 60000;
  EXPECT_EQ(triggerOf(s), Trigger::Continue);
  s.continuation = false;
  EXPECT_EQ(triggerOf(s), Trigger::None);
}

TEST(ReaderSyncScheduleEvents, ContinuationBeatsWake) {
  auto s = awakeHome();
  s.continuation = true;
  EXPECT_EQ(triggerOf(s), Trigger::Continue);
}

TEST(ReaderSyncScheduleManual, BypassesEnabledDeadlineAndBookOpeningButNotSafety) {
  auto s = awakeHome();
  s.enabled = false;
  s.wakePending = false;
  s.nextAttemptMs = s.nowMs + 900000;
  s.bookOpening = true;
  s.manualRequested = true;
  EXPECT_EQ(triggerOf(s), Trigger::Manual) << "the Sync now row is the explicit override";

  s.uiPermitted = false;
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s.uiPermitted = true;
  s.rateLimited = true;
  EXPECT_EQ(triggerOf(s), Trigger::None) << "server back-off wins over a manual request";
  s.nextAttemptMs = 0;
  EXPECT_EQ(triggerOf(s), Trigger::Manual) << "back-off elapsed";
}

TEST(ReaderSyncScheduleManual, WorksInBothModes) {
  auto s = awakeHome();
  s.manualRequested = true;
  s.schedule = Schedule::Interval;
  EXPECT_EQ(triggerOf(s), Trigger::Manual);
  s.schedule = Schedule::Events;
  EXPECT_EQ(triggerOf(s), Trigger::Manual);
}

TEST(ReaderSyncSchedulePreSleep, SyncsOnlyWhenUploadsArePending) {
  auto s = sleeping();
  EXPECT_EQ(triggerOf(s), Trigger::PreSleep);
  s.pendingUploads = false;
  EXPECT_EQ(triggerOf(s), Trigger::None) << "nothing to send: sleep immediately";
}

TEST(ReaderSyncSchedulePreSleep, RequiresEnabledEventsModeAndConfig) {
  auto s = sleeping();
  s.enabled = false;
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s = sleeping();
  s.configured = false;
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s = sleeping();
  s.schedule = Schedule::Interval;
  EXPECT_EQ(triggerOf(s), Trigger::None) << "interval mode keeps the legacy no-sleep-sync behaviour";
}

TEST(ReaderSyncSchedulePreSleep, ManualRequestDoesNotTurnItIntoAnotherTrigger) {
  auto s = sleeping();
  s.manualRequested = true;
  EXPECT_EQ(triggerOf(s), Trigger::PreSleep);
  s.pendingUploads = false;
  EXPECT_EQ(triggerOf(s), Trigger::None) << "sleep never runs a manual sync";
}

TEST(ReaderSyncSchedulePreSleep, RecentAttemptSkipsSoSleepIsNotRepeatedlyDelayed) {
  auto s = sleeping();
  s.lastAttemptAgeS = RETRY_DEBOUNCE_SECONDS - 1;
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s.lastAttemptAgeS = RETRY_DEBOUNCE_SECONDS;
  EXPECT_EQ(triggerOf(s), Trigger::PreSleep);
  s.lastAttemptAgeS = AGE_UNKNOWN;
  EXPECT_EQ(triggerOf(s), Trigger::PreSleep);
}

TEST(ReaderSyncSchedulePreSleep, IgnoresUiStateBecauseTheScreenIsAlreadyOff) {
  auto s = sleeping();
  s.uiPermitted = false;
  s.homeShown = false;
  s.bookOpening = true;
  s.nextAttemptMs = s.nowMs + 900000;  // an in-boot deadline is not a back-off
  EXPECT_EQ(triggerOf(s), Trigger::PreSleep);
}

TEST(ReaderSyncSchedulePreSleep, BlockedByBackoffRadioWorkerAndPark) {
  auto s = sleeping();
  s.rateLimited = true;
  s.nextAttemptMs = s.nowMs + 1000;
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s = sleeping();
  s.workerActive = true;
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s = sleeping();
  s.radioForeground = true;
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s = sleeping();
  s.parkedUntilWake = true;
  EXPECT_EQ(triggerOf(s), Trigger::None);
}

TEST(ReaderSyncScheduleInterval, KeepsTheLegacyFirstAttemptThenFixedPeriod) {
  auto s = awakeHome();
  s.schedule = Schedule::Interval;
  s.homeShown = false;  // legacy: any healthy screen counts
  EXPECT_EQ(triggerOf(s), Trigger::Wake);
  s.wakePending = false;
  s.nextAttemptMs = s.nowMs + 900000;
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s.nowMs += 899999;
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s.nowMs += 1;
  EXPECT_EQ(triggerOf(s), Trigger::Interval);
}

TEST(ReaderSyncScheduleInterval, IgnoresDebouncePowerAndBookOpening) {
  auto s = awakeHome();
  s.schedule = Schedule::Interval;
  s.lastSuccessAgeS = 10;
  s.bookOpening = true;
  EXPECT_EQ(triggerOf(s), Trigger::Wake) << "legacy behaviour has no debounce";
  s.wakePending = false;
  s.externalPower = true;
  s.nextAttemptMs = s.nowMs + 1;
  EXPECT_EQ(triggerOf(s), Trigger::None) << "charging does not shorten the interval";
}

TEST(ReaderSyncScheduleInterval, DisabledNeverSyncsExceptManually) {
  auto s = awakeHome();
  s.schedule = Schedule::Interval;
  s.enabled = false;
  EXPECT_EQ(triggerOf(s), Trigger::None);
  s.manualRequested = true;
  EXPECT_EQ(triggerOf(s), Trigger::Manual);
}

TEST(ReaderSyncWaitReason, ExplainsBlockedManualRequestAndReleasesAfterForegroundRadio) {
  auto s = awakeHome();
  s.manualRequested = true;
  s.radioForeground = true;
  EXPECT_EQ(explainDecision(s, decide(s)), WaitReason::WifiInUse);
  EXPECT_EQ(decide(s).trigger, Trigger::None);
  s.radioForeground = false;  // settings picker released its radio at the unlocked transition boundary
  EXPECT_EQ(decide(s).trigger, Trigger::Manual);
  EXPECT_EQ(explainDecision(s, decide(s)), WaitReason::Ready);
  s.rateLimited = true;
  s.nextAttemptMs = s.nowMs + 60000;
  EXPECT_EQ(explainDecision(s, decide(s)), WaitReason::Backoff);
  s.parkedUntilWake = true;
  EXPECT_EQ(explainDecision(s, decide(s)), WaitReason::UntilWake);
}

TEST(ReaderSyncWaitReason, ReportsActualPrerequisiteNotAnUnobservedScanResult) {
  auto s = awakeHome();
  EXPECT_EQ(explainDecision(s, decide(s), false), WaitReason::NoSavedNetwork);
  s.homeShown = false;
  EXPECT_EQ(explainDecision(s, decide(s)), WaitReason::Home);
  s.bookOpening = true;
  EXPECT_EQ(explainDecision(s, decide(s)), WaitReason::BookOpening);
  s.uiPermitted = false;
  EXPECT_EQ(explainDecision(s, decide(s)), WaitReason::Foreground);
  s.workerActive = true;
  EXPECT_EQ(explainDecision(s, decide(s)), WaitReason::WorkerActive);
}
