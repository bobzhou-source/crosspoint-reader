#include "ReaderSyncSettingsActivity.h"

#include <I18n.h>
#include <Memory.h>
#include <ctime>

#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "network/ReaderSync.h"

namespace fui = freeink::ui;
namespace {
constexpr StrId LABELS[] = {StrId::STR_READER_SYNC_ENABLED, StrId::STR_READER_SYNC_SCHEDULE,
    StrId::STR_READER_SYNC_NOW, StrId::STR_READER_SYNC_WIFI, StrId::STR_READER_SYNC_STATUS,
    StrId::STR_READER_SYNC_LAST, StrId::STR_READER_SYNC_BOOKS, StrId::STR_READER_SYNC_NOTES};
StrId phaseLabel(reader_sync::Phase phase) {
  using reader_sync::Phase;
  switch (phase) {
    case Phase::Unconfigured: return StrId::STR_READER_SYNC_UNCONFIGURED;
    case Phase::Disabled: return StrId::STR_STATE_OFF;
    case Phase::Waiting: return StrId::STR_READER_SYNC_WAITING;
    case Phase::Connecting: return StrId::STR_READER_SYNC_CONNECTING;
    case Phase::Syncing: return StrId::STR_READER_SYNC_RUNNING;
    case Phase::Success: return StrId::STR_READER_SYNC_SUCCESS;
    case Phase::Offline: return StrId::STR_READER_SYNC_OFFLINE;
    case Phase::InvalidConfig: return StrId::STR_READER_SYNC_CONFIG_ERROR;
    case Phase::ClockError: return StrId::STR_READER_SYNC_CLOCK_ERROR;
    case Phase::MemoryError: return StrId::STR_READER_SYNC_MEMORY_ERROR;
    case Phase::StorageError: return StrId::STR_READER_SYNC_STORAGE_ERROR;
    case Phase::AuthError: return StrId::STR_READER_SYNC_AUTH_ERROR;
    case Phase::RateLimited: return StrId::STR_READER_SYNC_RATE_LIMIT;
    case Phase::NeedsRedelivery: return StrId::STR_READER_SYNC_REDELIVERY;
    case Phase::AnnotationError: return StrId::STR_READER_SYNC_NOTE_ERROR;
    case Phase::Cancelled: return StrId::STR_READER_SYNC_PAUSED;
    default: return StrId::STR_READER_SYNC_PROTOCOL_ERROR;
  }
}
StrId waitLabel(reader_sync::WaitReason reason) {
  using reader_sync::WaitReason;
  switch (reason) {
    case WaitReason::WifiInUse:
      return StrId::STR_READER_SYNC_WAIT_WIFI;
    case WaitReason::Backoff:
      return StrId::STR_READER_SYNC_WAIT_BACKOFF;
    case WaitReason::UntilWake:
      return StrId::STR_READER_SYNC_WAIT_WAKE;
    case WaitReason::Home:
      return StrId::STR_READER_SYNC_WAIT_HOME;
    case WaitReason::BookOpening:
      return StrId::STR_READER_SYNC_WAIT_BOOK;
    case WaitReason::Foreground:
      return StrId::STR_READER_SYNC_WAIT_UI;
    case WaitReason::NoSavedNetwork:
      return StrId::STR_READER_SYNC_WAIT_NETWORK;
    case WaitReason::Ready:
      return StrId::STR_READER_SYNC_CONNECTING;
    case WaitReason::WorkerActive:
      return StrId::STR_READER_SYNC_RUNNING;
    case WaitReason::Disabled:
      return StrId::STR_STATE_OFF;
    case WaitReason::Unconfigured:
      return StrId::STR_READER_SYNC_UNCONFIGURED;
    default:
      return StrId::STR_READER_SYNC_WAIT_EVENT;
  }
}
}  // namespace

ReaderSyncSettingsActivity::ReaderSyncSettingsActivity(GfxRenderer& renderer, MappedInputManager& input)
    : UiListActivity("ReaderSyncSettings", renderer, input) {}
const char* ReaderSyncSettingsActivity::headerTitle() const { return tr(STR_READER_SYNC); }
void ReaderSyncSettingsActivity::onEnter() {
  UiListActivity::onEnter();
  for (int i = 0; i < ITEM_COUNT; ++i) {
    rows[i].label = I18N.get(LABELS[i]);
    rows[i].actionValue = i;
  }
}
void ReaderSyncSettingsActivity::loop() {
  UiListActivity::loop();
  const auto state = reader_sync::status();
  if (state.revision != revision) { revision = state.revision; requestUpdate(); }
}
void ReaderSyncSettingsActivity::activateIndex(int index) {
  nav.selected = index;
  app.clearTapFlash();
  if (index == 0) reader_sync::setEnabled(!reader_sync::status().enabled);
  else if (index == 1) {
    reader_sync::setSchedule(reader_sync::status().schedule == reader_sync::Schedule::Events
                                 ? reader_sync::Schedule::Interval
                                 : reader_sync::Schedule::Events);
  }
  else if (index == 2) reader_sync::syncNow();
  else if (index == 3) {
    // ActivityManager revokes and joins the worker before entering the selector.
    if (auto activity = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, false, false)) {
      startActivityForResult(std::move(activity), [](const ActivityResult&) {});
    }
  }
  requestUpdate();
}
void ReaderSyncSettingsActivity::buildScreen(UiScreen& screen) {
  const auto state = reader_sync::status();
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  rows[0].value = state.enabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  rows[1].value = state.schedule == reader_sync::Schedule::Events ? tr(STR_READER_SYNC_SCHEDULE_EVENTS)
                                                                   : tr(STR_READER_SYNC_SCHEDULE_INTERVAL);
  rows[2].value = state.active ? tr(STR_READER_SYNC_RUNNING) : "";
  rows[3].value = "";
  rows[4].value = I18N.get(phaseLabel(state.phase));
  if (state.phase == reader_sync::Phase::Waiting || state.phase == reader_sync::Phase::RateLimited ||
      (state.manualPending && !state.active)) {
    if (state.waitReason == reader_sync::WaitReason::Backoff) {
      snprintf(waitText, sizeof(waitText), tr(STR_READER_SYNC_WAIT_BACKOFF), static_cast<unsigned>(state.waitMinutes));
      rows[4].value = waitText;
    } else {
      rows[4].value = I18N.get(waitLabel(state.waitReason));
    }
  }
  lastSuccess[0] = '\0';
  if (state.lastSuccess > 0) {
    const time_t stamp = static_cast<time_t>(state.lastSuccess);
    struct tm date{};
    gmtime_r(&stamp, &date);
    strftime(lastSuccess, sizeof(lastSuccess), "%m-%d %H:%M UTC", &date);
  }
  rows[5].value = lastSuccess[0] ? lastSuccess : tr(STR_NOT_SET);
  snprintf(books, sizeof(books), "%lu", static_cast<unsigned long>(state.books));
  if (state.rejectedNotes) snprintf(notes, sizeof(notes), "%lu / %lu", static_cast<unsigned long>(state.notes), static_cast<unsigned long>(state.rejectedNotes));
  else snprintf(notes, sizeof(notes), "%lu", static_cast<unsigned long>(state.notes));
  rows[6].value = books;
  rows[7].value = notes;
  fui::ListProps props;
  props.items = rows;
  props.count = ITEM_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
