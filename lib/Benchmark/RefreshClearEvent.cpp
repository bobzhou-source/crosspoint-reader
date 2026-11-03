#include "RefreshClearEvent.h"

namespace benchmark {

ClearCause clearCause(const AdaptiveRefreshPolicy::ClearRecord& record) {
  using Reason = AdaptiveRefreshPolicy::Reason;
  using Source = AdaptiveRefreshPolicy::ClearSource;
  if (record.source == Source::Manual) return ClearCause::Manual;
  switch (record.reason) {
    case Reason::Initial:
      return ClearCause::Initial;
    case Reason::Debt:
      return ClearCause::Debt;
    case Reason::SafetyCap:
      return ClearCause::SafetyCap;
    case Reason::PolarityChange:
      return ClearCause::Polarity;
    case Reason::InvalidFrame:
      return ClearCause::InvalidFrame;
    case Reason::External:
      return ClearCause::Overlay;
    case Reason::Requested:
      if (record.source == Source::Image) return ClearCause::Image;
      if (record.source == Source::Popup) return ClearCause::Popup;
      if (record.source == Source::External) return ClearCause::Overlay;
      return ClearCause::Requested;
    case Reason::None:
      break;
  }
  return ClearCause::Unlabelled;
}

Event refreshClearEvent(const AdaptiveRefreshPolicy::ClearRecord& record, const bool policyFile, const uint32_t bookId,
                        const uint64_t nowUs) {
  Event event;
  event.type = record.manual() ? EventType::ManualRefresh : EventType::RefreshClear;
  event.startUs = event.endUs = nowUs;
  event.data[0] = static_cast<uint32_t>(clearCause(record));
  event.data[1] = record.fastTurns;
  event.data[2] = record.debtPermille;
  event.data[3] = record.imagePages;
  event.data[4] = record.grayPages;
  event.data[5] = (record.inverted ? CLEAR_FLAG_NIGHT : 0) | (policyFile ? CLEAR_FLAG_POLICY_FILE : 0) |
                  (record.image ? CLEAR_FLAG_IMAGE : 0) | (record.grayscale ? CLEAR_FLAG_GRAYSCALE : 0);
  event.data[6] = bookId;
  event.data[7] = record.thresholdPermille;
  return event;
}

}  // namespace benchmark
