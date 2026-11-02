#include "AnnotationReviewActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "AnnotationEditorActivity.h"
#include "AnnotationNavigation.h"
#include "AnnotationStore.h"
#include "ReaderTextSlices.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;
namespace {
constexpr StrId KIND_LABELS[] = {StrId::STR_ANNOTATION_QUOTE, StrId::STR_ANNOTATION_QUESTION,
                                StrId::STR_ANNOTATION_REVIEW_LATER};
constexpr StrId CARD_QUESTIONS[] = {StrId::STR_STUDY_QUOTE_PROMPT, StrId::STR_STUDY_QUESTION_PROMPT,
                                   StrId::STR_STUDY_REVIEW_PROMPT};
}  // namespace

AnnotationReviewActivity::AnnotationReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::unique_ptr<AnnotationEntry> entry)
    : UiListActivity("AnnotationReview", renderer, mappedInput), annotation(std::move(entry)) {}

AnnotationReviewActivity::AnnotationReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::unique_ptr<StudyCardEntry> card)
    : UiListActivity("StudyCard", renderer, mappedInput), card(std::move(card)) {}

const AnnotationEntry* AnnotationReviewActivity::source() const { return card ? &card->source : annotation.get(); }

void AnnotationReviewActivity::onEnter() {
  rebuildRows();
  UiListActivity::onEnter();
}

void AnnotationReviewActivity::onExit() {
  annotation.reset();
  card.reset();
  rowCount = 0;
  UiListActivity::onExit();
}

void AnnotationReviewActivity::appendText(const char* heading, const char* text) {
  if (!text || !*text) text = tr(STR_REVIEW_NOT_RECORDED);
  if (fieldCount < fields.size()) fields[fieldCount++] = {text, heading, 0};
}

void AnnotationReviewActivity::rebuildRows() {
  rowCount = 0;
  fieldCount = 0;
  rowsDirty = true;
  const auto* entry = source();
  loaded = entry && static_cast<uint8_t>(entry->kind) < 3;
  if (!loaded) return;
  if (card) {
    appendText(revealed ? tr(STR_STUDY_ANSWER) : tr(STR_ANNOTATION_QUESTION),
               revealed ? entry->excerpt.c_str() : card->front.c_str());
  }
  appendText(tr(STR_ANNOTATION_KIND), I18N.get(KIND_LABELS[static_cast<uint8_t>(entry->kind)]));
  appendText(tr(STR_REVIEW_SOURCE_TITLE), entry->sourceTitle.c_str());
  appendText(tr(STR_CHAPTER), entry->chapterTitle.c_str());
  if (!card || revealed) {
    if (!card) {
      appendText(entry->pageContext ? tr(STR_ANNOTATION_PAGE_CONTEXT) : tr(STR_ANNOTATION_EXCERPT), entry->excerpt.c_str());
    }
    appendText(tr(STR_ANNOTATION_NOTE), entry->note.empty() ? tr(STR_REVIEW_NO_NOTE) : entry->note.c_str());
  }
  appendText(tr(STR_REVIEW_SOURCE_PATH), entry->sourcePath.c_str());
  appendText(tr(STR_REVIEW_CHAPTER_LINK), entry->chapterHref.c_str());
  snprintf(savedAt, sizeof(savedAt), "%llu", static_cast<unsigned long long>(entry->createdAt));
  appendText(tr(STR_REVIEW_CREATED_AT), entry->createdAt == 0 ? tr(STR_REVIEW_CLOCK_UNSET) : savedAt);
  nav.follow(listCount());
}

int AnnotationReviewActivity::listCount() const { return loaded ? static_cast<int>(3 + rowCount) : 0; }

const char* AnnotationReviewActivity::headerTitle() const {
  return card ? (revealed ? tr(STR_STUDY_BACK) : tr(STR_STUDY_FRONT)) : tr(STR_REVIEW_ANNOTATION);
}

void AnnotationReviewActivity::provideRow(void* context, const uint16_t index, fui::ListItem& item) {
  auto& self = *static_cast<AnnotationReviewActivity*>(context);
  item.actionValue = static_cast<int16_t>(index);
  if (index == 0) {
    item.label = self.card ? (self.revealed ? tr(STR_STUDY_HIDE) : tr(STR_STUDY_REVEAL)) : tr(STR_REVIEW_OPEN_SOURCE);
  } else if (index == 1) {
    item.label = self.card ? tr(STR_REVIEW_OPEN_SOURCE) : tr(STR_STUDY_CREATE);
  } else if (index == 2) {
    item.label = self.card ? tr(STR_STUDY_DELETE) : tr(STR_REVIEW_EDIT);
  } else if (index - 3 < self.rowCount) {
    size_t rowIndex = index - 3;
    for (size_t i = 0; i < self.fieldCount; ++i) {
      auto& field = self.fields[i];
      if (rowIndex >= field.rowCount) {
        rowIndex -= field.rowCount;
        continue;
      }
      ReaderTextSlices::copyRow(*self.textTarget, field.text, self.textStyle, self.textWidth,
                               rowIndex, self.rowText, sizeof(self.rowText), field.cursor);
      item.label = self.rowText;
      item.sectionHeading = rowIndex == 0 ? field.heading : nullptr;
      break;
    }
  }
}

void AnnotationReviewActivity::buildScreen(UiScreen& screen) {
  const Rect body = UITheme::getContentBounds(renderer);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(body.y),
                  static_cast<int16_t>(renderer.getScreenWidth() - (body.x + body.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (body.y + body.height)),
                  static_cast<int16_t>(body.x)});
  if (!loaded) {
    auto style = screen.theme().bodyText;
    style.align = fui::TextAlign::Center;
    style.maxLines = 4;
    screen.target().text(screen.body(), tr(STR_ANNOTATION_LOAD_FAILED), style);
    return;
  }
  fui::ListProps props;
  props.rowProvider = &AnnotationReviewActivity::provideRow;
  props.rowProviderCtx = this;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = ReaderTextSlices::LINES;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.rowInset = 0;
  props = screen.resolveListProps(props);
  const int16_t strip = props.scrollIndicator
                            ? std::max(0, props.scrollIndicatorWidth + props.scrollIndicatorInset + 2 - props.rowInset)
                            : 0;
  const int16_t width = static_cast<int16_t>(screen.body().width - 2 * (props.rowInset + props.sidePadding) - strip);
  textTarget = &screen.target();
  if (rowsDirty || width != textWidth || textStyle.font != props.labelText.font) {
    textWidth = std::max<int16_t>(1, width);
    textStyle = props.labelText;
    rowCount = 0;
    for (size_t i = 0; i < fieldCount; ++i) {
      fields[i].cursor = {};
      fields[i].rowCount = static_cast<uint16_t>(
          ReaderTextSlices::count(*textTarget, fields[i].text, textStyle, textWidth));
      rowCount += fields[i].rowCount;
    }
    rowsDirty = false;
  }
  props.count = static_cast<uint16_t>(listCount());
  syncListViewport(screen, props);
  screen.list(props);
}

void AnnotationReviewActivity::activateIndex(const int index) {
  if (!loaded || popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (card) {
    if (index == 0) {
      RenderLock lock;
      revealed = !revealed;
      rebuildRows();
      nav.reset();
      requestUpdate();
    } else if (index == 1) {
      openSource();
    } else if (index == 2) {
      deleteCard();
    }
  } else if (index == 0) {
    openSource();
  } else if (index == 1) {
    createCard();
  } else if (index == 2) {
    editAnnotation();
  }
}

void AnnotationReviewActivity::openSource() {
  if (!source() || !Storage.ready() || !Storage.exists(source()->sourcePath.c_str())) {
    showMessage(StrId::STR_REVIEW_SOURCE_MISSING);
    return;
  }
  setResult(ActivityResult{annotationProgress(*source())});
  finish();
}

void AnnotationReviewActivity::createCard() {
  auto created = makeUniqueNoThrow<StudyCardEntry>();
  if (!created) {
    LOG_ERR("STUDY", "OOM: card snapshot");
    showMessage(StrId::STR_STUDY_SAVE_FAILED);
    return;
  }
  const auto status = StudyCardStore::create(annotation->sourcePath, annotation->id,
                                            I18N.get(CARD_QUESTIONS[static_cast<uint8_t>(annotation->kind)]), *created);
  if (status != StudyCardStore::Result::Success) {
    showMessage(status == StudyCardStore::Result::LimitReached ? StrId::STR_STUDY_LIMIT : StrId::STR_STUDY_SAVE_FAILED);
    return;
  }
  auto review = makeUniqueNoThrow<AnnotationReviewActivity>(renderer, mappedInput, std::move(created));
  if (!review) {
    LOG_ERR("STUDY", "OOM: saved card review");
    showMessage(StrId::STR_STUDY_SAVED);
    return;
  }
  startActivityForResult(std::move(review), [this](const ActivityResult& result) {
    if (!result.isCancelled && std::holds_alternative<ProgressChangeResult>(result.data)) {
      setResult(ActivityResult(result));
      finish();
    }
  });
}

void AnnotationReviewActivity::editAnnotation() {
  auto copy = makeUniqueNoThrow<AnnotationEntry>(*annotation);
  if (!copy) {
    LOG_ERR("STUDY", "OOM: annotation edit");
    showMessage(StrId::STR_ANNOTATION_LOAD_FAILED);
    return;
  }
  auto editor = makeUniqueNoThrow<AnnotationEditorActivity>(renderer, mappedInput, std::move(copy));
  if (!editor) {
    LOG_ERR("STUDY", "OOM: annotation editor");
    showMessage(StrId::STR_ANNOTATION_LOAD_FAILED);
    return;
  }
  startActivityForResult(std::move(editor), [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    RenderLock lock;
    const std::string bookPath = annotation->sourcePath;
    const auto status = AnnotationStore::load(bookPath, annotation->id, *annotation);
    if (status == AnnotationStore::Result::NotFound) {
      finish();
      return;
    }
    if (status != AnnotationStore::Result::Success) {
      loaded = false;
      rowCount = 0;
    } else {
      rebuildRows();
    }
  });
}

void AnnotationReviewActivity::deleteCard() {
  RenderLock lock;
  const StrId options[] = {StrId::STR_CANCEL, StrId::STR_DELETE};
  popup.show(StrId::STR_STUDY_DELETE_CONFIRM, options, 2, 0, [this](const int selected) {
    if (selected != 1) return;
    if (StudyCardStore::remove(card->source.sourcePath, card->id) != StudyCardStore::Result::Success) {
      showMessage(StrId::STR_STUDY_DELETE_FAILED);
      return;
    }
    finish();
  });
  requestUpdate();
}

void AnnotationReviewActivity::showMessage(const StrId message) {
  RenderLock lock;
  const StrId options[] = {StrId::STR_OK_BUTTON};
  popup.show(message, options, 1, 0, [](int) {});
  requestUpdate();
}

bool AnnotationReviewActivity::handleCustomInput() {
  return popup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void AnnotationReviewActivity::render(RenderLock&& lock) {
  if (popup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
