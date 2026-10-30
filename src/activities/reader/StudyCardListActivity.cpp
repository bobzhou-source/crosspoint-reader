#include "StudyCardListActivity.h"

#include <I18n.h>
#include <Memory.h>

#include <cstdio>

#include "AnnotationReviewActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;
namespace {
constexpr StrId KIND_LABELS[] = {StrId::STR_ANNOTATION_QUOTE, StrId::STR_ANNOTATION_QUESTION,
                                StrId::STR_ANNOTATION_REVIEW_LATER};
}  // namespace

StudyCardListActivity::StudyCardListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::string bookPath)
    : UiListActivity("StudyCards", renderer, mappedInput), bookPath(std::move(bookPath)) {}

void StudyCardListActivity::onEnter() {
  // One bounded summary index, not all source snapshots, for the screen lifetime.
  entries = makeUniqueNoThrow<StudyCardSummary[]>(StudyCardStore::MAX_ENTRIES);
  if (!entries) LOG_ERR("STUDY", "OOM: card index");
  reload();
  UiListActivity::onEnter();
}

void StudyCardListActivity::onExit() {
  entries.reset();
  count = 0;
  UiListActivity::onExit();
}

void StudyCardListActivity::reload() {
  loaded = entries && StudyCardStore::list(bookPath, entries.get(), StudyCardStore::MAX_ENTRIES, count) ==
                          StudyCardStore::Result::Success;
  if (!loaded) count = 0;
  nav.follow(listCount());
}

int StudyCardListActivity::listCount() const { return loaded ? static_cast<int>(count) : 0; }

const char* StudyCardListActivity::headerTitle() const { return tr(STR_STUDY_CARDS); }

void StudyCardListActivity::provideRow(void* context, const uint16_t index, fui::ListItem& item) {
  auto& self = *static_cast<StudyCardListActivity*>(context);
  if (index >= self.count) return;
  const auto& entry = self.entries[index];
  item.actionValue = static_cast<int16_t>(index);
  item.label = entry.front;
  snprintf(self.rowSubtitle, sizeof(self.rowSubtitle), "#%u - %s %u", static_cast<unsigned>(entry.id), tr(STR_CHAPTER),
           static_cast<unsigned>(entry.spineIndex) + 1);
  item.subtitle = self.rowSubtitle;
  item.value = I18N.get(KIND_LABELS[static_cast<uint8_t>(entry.kind)]);
}

void StudyCardListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (!loaded || count == 0) {
    screen.centeredText(loaded ? tr(STR_STUDY_EMPTY) : tr(STR_STUDY_LOAD_FAILED), screen.theme().bodyText);
    return;
  }
  fui::ListProps props;
  props.rowProvider = &StudyCardListActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText.maxLines = 3;
  syncListViewport(screen, props);
  screen.list(props);
}

void StudyCardListActivity::activateIndex(const int index) {
  if (!loaded || popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  auto card = makeUniqueNoThrow<StudyCardEntry>();
  if (!card) {
    LOG_ERR("STUDY", "OOM: card");
    showLoadError();
    return;
  }
  if (StudyCardStore::load(bookPath, entries[index].id, *card) != StudyCardStore::Result::Success) {
    showLoadError();
    return;
  }
  auto review = makeUniqueNoThrow<AnnotationReviewActivity>(renderer, mappedInput, std::move(card));
  if (!review) {
    LOG_ERR("STUDY", "OOM: card review");
    showLoadError();
    return;
  }
  startActivityForResult(std::move(review), [this](const ActivityResult& result) {
    if (!result.isCancelled && std::holds_alternative<ProgressChangeResult>(result.data)) {
      setResult(ActivityResult(result));
      finish();
      return;
    }
    RenderLock lock;
    reload();
  });
}

void StudyCardListActivity::showLoadError() {
  RenderLock lock;
  const StrId options[] = {StrId::STR_OK_BUTTON};
  popup.show(StrId::STR_STUDY_LOAD_FAILED, options, 1, 0, [](int) {});
  requestUpdate();
}

bool StudyCardListActivity::handleCustomInput() {
  return popup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void StudyCardListActivity::render(RenderLock&& lock) {
  if (popup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
