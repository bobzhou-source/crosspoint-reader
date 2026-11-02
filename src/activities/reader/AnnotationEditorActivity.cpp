#include "AnnotationEditorActivity.h"

#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "AnnotationStore.h"
#include "ReaderTextSlices.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr StrId KIND_LABELS[] = {StrId::STR_ANNOTATION_QUOTE, StrId::STR_ANNOTATION_QUESTION,
                                StrId::STR_ANNOTATION_REVIEW_LATER};
}  // namespace

AnnotationEditorActivity::AnnotationEditorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::unique_ptr<AnnotationEntry> entry)
    : UiListActivity("AnnotationEditor", renderer, mappedInput), entry(std::move(entry)) {}

void AnnotationEditorActivity::onEnter() {
  contextRowCount = 0;
  if (entry) {
    initialNote = entry->note;
    initialKind = entry->kind;
    contextWidth = 0;
  }
  UiListActivity::onEnter();
}

void AnnotationEditorActivity::onExit() {
  entry.reset();
  UiListActivity::onExit();
}

int AnnotationEditorActivity::listCount() const {
  return entry ? static_cast<int>(3 + contextRowCount + (entry->id != 0 ? 1 : 0)) : 0;
}

const char* AnnotationEditorActivity::headerTitle() const { return tr(STR_ANNOTATIONS); }

void AnnotationEditorActivity::provideRow(void* context, const uint16_t index, fui::ListItem& item) {
  auto& self = *static_cast<AnnotationEditorActivity*>(context);
  if (!self.entry) return;
  item.actionValue = static_cast<int16_t>(index);
  if (index == 0) {
    item.label = tr(STR_SAVE_ANNOTATION);
    item.subtitle = self.entry->sourceTitle.c_str();
  } else if (index == 1) {
    item.label = tr(STR_ANNOTATION_KIND);
    item.value = I18N.get(KIND_LABELS[static_cast<uint8_t>(self.entry->kind)]);
  } else if (index == 2) {
    item.label = tr(STR_EDIT_NOTE);
    item.subtitle = self.entry->note.empty() ? nullptr : self.entry->note.c_str();
  } else if (index < 3 + self.contextRowCount) {
    ReaderTextSlices::copyRow(*self.contextTarget, self.entry->excerpt.c_str(), self.contextStyle,
                             self.contextWidth, index - 3, self.contextText, sizeof(self.contextText), self.contextCursor);
    item.label = self.contextText;
    if (index == 3) {
      item.sectionHeading = self.entry->pageContext ? tr(STR_ANNOTATION_PAGE_CONTEXT) : tr(STR_ANNOTATION_EXCERPT);
    }
  } else {
    item.label = tr(STR_DELETE);
  }
}

void AnnotationEditorActivity::buildScreen(UiScreen& screen) {
  const Rect body = UITheme::getContentBounds(renderer);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(body.y),
                  static_cast<int16_t>(renderer.getScreenWidth() - (body.x + body.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (body.y + body.height)),
                  static_cast<int16_t>(body.x)});
  if (!entry) {
    auto style = screen.theme().bodyText;
    style.align = fui::TextAlign::Center;
    style.maxLines = 4;
    screen.target().text(screen.body(), tr(STR_ANNOTATION_LOAD_FAILED), style);
    return;
  }
  fui::ListProps props;
  props.rowProvider = &AnnotationEditorActivity::provideRow;
  props.rowProviderCtx = this;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = ReaderTextSlices::LINES;
  props.subtitleText = screen.theme().smallText;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.rowInset = 0;
  props = screen.resolveListProps(props);
  const int16_t strip = props.scrollIndicator
                            ? std::max(0, props.scrollIndicatorWidth + props.scrollIndicatorInset + 2 - props.rowInset)
                            : 0;
  const int16_t width = static_cast<int16_t>(screen.body().width - 2 * (props.rowInset + props.sidePadding) - strip);
  contextTarget = &screen.target();
  if (width != contextWidth || contextStyle.font != props.labelText.font) {
    contextWidth = std::max<int16_t>(1, width);
    contextStyle = props.labelText;
    contextCursor = {};
    contextRowCount = ReaderTextSlices::count(*contextTarget, entry->excerpt.c_str(), contextStyle, contextWidth);
  }
  props.count = static_cast<uint16_t>(listCount());
  props.subtitleText.maxLines = 3;
  syncListViewport(screen, props);
  screen.list(props);
}

void AnnotationEditorActivity::activateIndex(const int index) {
  if (!entry || popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (index == 0) {
    saveAnnotation();
  } else if (index == 1) {
    RenderLock lock;
    popup.show(StrId::STR_ANNOTATION_KIND, KIND_LABELS, 3, static_cast<int>(entry->kind), [this](const int selected) {
      if (selected < 0 || selected >= 3) return;
      RenderLock lock;
      entry->kind = static_cast<AnnotationKind>(selected);
    });
    requestUpdate();
  } else if (index == 2) {
    editNote();
  } else if (index == static_cast<int>(3 + contextRowCount)) {
    confirmDelete();
  }
}

bool AnnotationEditorActivity::handleCustomInput() {
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (saved) {
    finish();
    return true;
  }
  return false;
}

void AnnotationEditorActivity::showMessage(const StrId message) {
  RenderLock lock;
  const StrId options[] = {StrId::STR_OK_BUTTON};
  popup.show(message, options, 1, 0, [](int) {});
  requestUpdate();
}

void AnnotationEditorActivity::editNote() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ANNOTATION_NOTE), entry->note,
                                                          AnnotationEntry::MAX_NOTE_BYTES, InputType::Text);
  if (!keyboard) {
    LOG_ERR("ANN", "OOM: note keyboard");
    showMessage(StrId::STR_ANNOTATION_SAVE_FAILED);
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) return;
    auto note = annotationText(std::get<KeyboardResult>(result.data).text, AnnotationEntry::MAX_NOTE_BYTES);
    RenderLock lock;
    entry->note = std::move(note);
  });
}

bool AnnotationEditorActivity::hasUnsavedChanges() const {
  return entry && (entry->id == 0 || entry->note != initialNote || entry->kind != initialKind);
}

void AnnotationEditorActivity::saveAnnotation() {
  if (!hasUnsavedChanges()) {
    finish();
    return;
  }
  AnnotationStore::Result status;
  {
    RenderLock lock;
    status = AnnotationStore::save(*entry);
  }
  if (status != AnnotationStore::Result::Success) {
    showMessage(status == AnnotationStore::Result::LimitReached ? StrId::STR_ANNOTATION_LIMIT
                                                                : StrId::STR_ANNOTATION_SAVE_FAILED);
    return;
  }
  saved = true;
  setResult(ActivityResult{});
  showMessage(StrId::STR_ANNOTATION_SAVED);
}

void AnnotationEditorActivity::confirmDelete() {
  RenderLock lock;
  const StrId options[] = {StrId::STR_CANCEL, StrId::STR_DELETE};
  popup.show(StrId::STR_DELETE_ANNOTATION, options, 2, 0, [this](const int selected) {
    if (selected != 1) return;
    if (AnnotationStore::remove(entry->sourcePath, entry->id) != AnnotationStore::Result::Success) {
      showMessage(StrId::STR_ANNOTATION_DELETE_FAILED);
      return;
    }
    setResult(ActivityResult{});
    finish();
  });
  requestUpdate();
}

void AnnotationEditorActivity::onBackButton() {
  if (!hasUnsavedChanges()) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  RenderLock lock;
  const StrId options[] = {StrId::STR_CANCEL, StrId::STR_CONFIRM};
  popup.show(StrId::STR_ANNOTATION_DISCARD, options, 2, 0, [this](const int selected) {
    if (selected != 1) return;
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  });
  requestUpdate();
}

void AnnotationEditorActivity::render(RenderLock&& lock) {
  if (popup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
