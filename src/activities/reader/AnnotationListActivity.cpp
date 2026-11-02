#include "AnnotationListActivity.h"

#include <I18n.h>
#include <Memory.h>

#include <cstdio>

#include "AnnotationReviewActivity.h"
#include "AnnotationStore.h"
#include "StudyCardListActivity.h"
#include "components/UITheme.h"
#include "util/AnnotationExport.h"

namespace fui = freeink::ui;

namespace {
constexpr StrId KIND_LABELS[] = {StrId::STR_ANNOTATION_QUOTE, StrId::STR_ANNOTATION_QUESTION,
                                StrId::STR_ANNOTATION_REVIEW_LATER};
}  // namespace

AnnotationListActivity::AnnotationListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               std::string bookPath, std::string title)
    : UiListActivity("AnnotationList", renderer, mappedInput), bookPath(std::move(bookPath)), title(std::move(title)) {}

void AnnotationListActivity::onEnter() {
  // Fixed ~16 KiB index, rather than 128 full records. Rows are provided on demand
  // into a reused subtitle buffer; navigation/rendering performs no allocation or I/O.
  entries = makeUniqueNoThrow<AnnotationSummary[]>(AnnotationStore::MAX_ENTRIES);
  if (!entries) LOG_ERR("ANN", "OOM: annotation list");
  reload();
  UiListActivity::onEnter();
}

void AnnotationListActivity::onExit() {
  entries.reset();
  count = 0;
  UiListActivity::onExit();
}

void AnnotationListActivity::reload() {
  loaded = entries && AnnotationStore::list(bookPath, entries.get(), AnnotationStore::MAX_ENTRIES, count) ==
                          AnnotationStore::Result::Success;
  if (!loaded) count = 0;
  nav.follow(listCount());
}

int AnnotationListActivity::listCount() const { return loaded ? static_cast<int>(count + 2) : 1; }

const char* AnnotationListActivity::headerTitle() const { return tr(STR_ANNOTATIONS); }

void AnnotationListActivity::provideRow(void* context, const uint16_t index, fui::ListItem& item) {
  auto& self = *static_cast<AnnotationListActivity*>(context);
  item.actionValue = static_cast<int16_t>(index);
  if (index == 0) {
    item.label = tr(STR_STUDY_CARDS);
    item.subtitle = self.title.c_str();
    return;
  }
  if (index == 1) {
    item.label = tr(STR_EXPORT_ANNOTATIONS);
    item.subtitle = self.title.c_str();
    return;
  }
  if (index - 2 >= self.count) return;
  const auto& summary = self.entries[index - 2];
  item.label = summary.label;
  snprintf(self.rowSubtitle, sizeof(self.rowSubtitle), "%u%% - %s %u",
           static_cast<unsigned>(summary.percentage * 100.0f + 0.5f), tr(STR_CHAPTER),
           static_cast<unsigned>(summary.spineIndex) + 1);
  item.subtitle = self.rowSubtitle;
  item.value = I18N.get(KIND_LABELS[static_cast<uint8_t>(summary.kind)]);
}

void AnnotationListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                  static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  auto stateStyle = screen.theme().bodyText;
  stateStyle.maxLines = 3;
  if (!loaded) {
    screen.centeredText(tr(STR_ANNOTATION_LOAD_FAILED), stateStyle);
  }
  if (loaded && count == 0) {
    screen.centeredText(tr(STR_NO_ANNOTATIONS), stateStyle);
    screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  }
  fui::ListProps props;
  props.rowProvider = &AnnotationListActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.rowHeight = 72;
  syncListViewport(screen, props);
  screen.list(props);
}

void AnnotationListActivity::activateIndex(const int index) {
  if (popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (index == 0) {
    openStudyCards();
    return;
  }
  if (!loaded) return;
  if (index == 1) {
    if (count == 0) {
      showMessage(StrId::STR_NO_ANNOTATIONS);
    } else {
      exportAnnotations();
    }
    return;
  }
  auto entry = makeUniqueNoThrow<AnnotationEntry>();
  if (!entry) {
    LOG_ERR("ANN", "OOM: annotation entry");
    showMessage(StrId::STR_ANNOTATION_LOAD_FAILED);
    return;
  }
  if (AnnotationStore::load(bookPath, entries[index - 2].id, *entry) != AnnotationStore::Result::Success) {
    showMessage(StrId::STR_ANNOTATION_LOAD_FAILED);
    return;
  }
  auto editor = makeUniqueNoThrow<AnnotationReviewActivity>(renderer, mappedInput, std::move(entry));
  if (!editor) {
    LOG_ERR("ANN", "OOM: annotation review");
    showMessage(StrId::STR_ANNOTATION_LOAD_FAILED);
    return;
  }
  startActivityForResult(std::move(editor), [this](const ActivityResult& result) {
    if (!result.isCancelled && std::holds_alternative<ProgressChangeResult>(result.data)) {
      setResult(ActivityResult(result));
      finish();
      return;
    }
    RenderLock lock;
    reload();
  });
}

void AnnotationListActivity::openStudyCards() {
  auto cards = makeUniqueNoThrow<StudyCardListActivity>(renderer, mappedInput, bookPath);
  if (!cards) {
    LOG_ERR("ANN", "OOM: study deck");
    showMessage(StrId::STR_STUDY_LOAD_FAILED);
    return;
  }
  {
    RenderLock lock;
    // Do not pin both the annotation and card indexes while reviewing a deck.
    entries.reset();
    count = 0;
    loaded = false;
  }
  startActivityForResult(std::move(cards), [this](const ActivityResult& result) {
    if (!result.isCancelled && std::holds_alternative<ProgressChangeResult>(result.data)) {
      setResult(ActivityResult(result));
      finish();
      return;
    }
    RenderLock lock;
    entries = makeUniqueNoThrow<AnnotationSummary[]>(AnnotationStore::MAX_ENTRIES);
    if (!entries) LOG_ERR("ANN", "OOM: annotation index");
    reload();
  });
}

void AnnotationListActivity::exportAnnotations() {
  std::string outputPath;
  if (!AnnotationExport::writeMarkdown(bookPath, title, entries.get(), count, outputPath)) {
    showMessage(StrId::STR_ANNOTATION_EXPORT_FAILED);
    return;
  }
  showMessage(StrId::STR_ANNOTATION_EXPORTED, outputPath.c_str());
}

void AnnotationListActivity::showMessage(const StrId message, const char* detail) {
  RenderLock lock;
  const char* options[] = {tr(STR_OK_BUTTON)};
  popup.show(I18N.get(message), detail, options, 1, 0, [](int) {});
  requestUpdate();
}

bool AnnotationListActivity::handleCustomInput() {
  return popup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void AnnotationListActivity::render(RenderLock&& lock) {
  if (popup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
