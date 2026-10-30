#include "ReadingInboxActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "activities/reader/ReaderActivity.h"
#include "components/UITheme.h"
#include "util/AnnotationExport.h"

namespace fui = freeink::ui;

namespace {
constexpr StrId KIND_LABELS[] = {StrId::STR_ANNOTATION_QUOTE, StrId::STR_ANNOTATION_QUESTION,
                                StrId::STR_ANNOTATION_REVIEW_LATER};
}  // namespace

ReadingInboxActivity::ReadingInboxActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ReadingInbox", renderer, mappedInput) {}

void ReadingInboxActivity::onEnter() {
  reload();
  UiListActivity::onEnter();
}

void ReadingInboxActivity::onExit() {
  rows.reset();
  count = total = 0;
  UiListActivity::onExit();
}

AnnotationStore::Result ReadingInboxActivity::collectEntry(const AnnotationEntry& entry, void* context) {
  auto& self = *static_cast<ReadingInboxActivity*>(context);
  const size_t ordinal = self.total++;
  if (ordinal < self.pageStart || self.count == PAGE_SIZE) return AnnotationStore::Result::Success;
  auto& row = self.rows[self.count++];
  const std::string key = AnnotationStore::sourceKey(entry.sourcePath);
  memcpy(row.sourceKey, key.c_str(), key.size() + 1);
  row.id = entry.id;
  row.kind = entry.kind;
  row.percentage = static_cast<uint8_t>(entry.location.percentage * 100.0f + 0.5f);
  const std::string title = annotationText(entry.sourceTitle.empty() ? entry.sourcePath : entry.sourceTitle,
                                         sizeof(row.title) - 1);
  memcpy(row.title, title.c_str(), title.size() + 1);
  std::string_view filename(entry.sourcePath);
  const size_t slash = filename.find_last_of("/\\");
  if (slash != std::string_view::npos) filename.remove_prefix(slash + 1);
  const std::string source = annotationText(filename, 80);
  const std::string chapter = annotationText(entry.chapterTitle.empty() ? entry.chapterHref : entry.chapterTitle, 108);
  snprintf(row.sourceChapter, sizeof(row.sourceChapter), "%s - %s", source.c_str(), chapter.c_str());
  return AnnotationStore::Result::Success;
}

void ReadingInboxActivity::reload() {
  // About 10 KiB, allocated once per screen. Full records and JSON are only read
  // on entry or explicit refresh/page actions, never in row providers/render.
  if (!rows) rows = makeUniqueNoThrow<Row[]>(PAGE_SIZE);
  if (!rows) LOG_ERR("INBOX", "OOM: inbox page index");
  count = total = 0;
  loaded = rows && AnnotationStore::visitAll(&ReadingInboxActivity::collectEntry, this) ==
                       AnnotationStore::Result::Success;
  if (!loaded) count = total = 0;
  // Files may have been removed externally between page requests.
  if (loaded && pageStart > 0 && pageStart >= total) {
    pageStart = 0;
    count = total = 0;
    loaded = AnnotationStore::visitAll(&ReadingInboxActivity::collectEntry, this) == AnnotationStore::Result::Success;
    if (!loaded) count = total = 0;
  }
  snprintf(pageLabel, sizeof(pageLabel), tr(STR_READING_INBOX_RANGE),
           static_cast<unsigned long>(count ? pageStart + 1 : 0), static_cast<unsigned long>(pageStart + count),
           static_cast<unsigned long>(total));
  nav.reset();
  nav.follow(listCount());
}

int ReadingInboxActivity::listCount() const {
  if (!loaded) return 1;  // Refresh remains available after a failed scan.
  return static_cast<int>(2 + count + (pageStart > 0 ? 1 : 0) + (pageStart + count < total ? 1 : 0));
}

const char* ReadingInboxActivity::headerTitle() const { return tr(STR_READING_INBOX); }

void ReadingInboxActivity::provideRow(void* context, const uint16_t index, fui::ListItem& item) {
  auto& self = *static_cast<ReadingInboxActivity*>(context);
  item.actionValue = static_cast<int16_t>(index);
  if (index == 0) {
    item.label = tr(STR_READING_INBOX_REFRESH);
    item.subtitle = self.loaded ? self.pageLabel : tr(STR_ANNOTATION_LOAD_FAILED);
    return;
  }
  if (index == 1) {
    item.label = tr(STR_READING_INBOX_EXPORT);
    item.subtitle = tr(STR_READING_INBOX_LOCAL);
    return;
  }
  const size_t rowIndex = index - 2;
  if (rowIndex < self.count) {
    const auto& row = self.rows[rowIndex];
    item.label = row.title;
    item.subtitle = row.sourceChapter;
    snprintf(self.rowValue, sizeof(self.rowValue), "%s - %u%%", I18N.get(KIND_LABELS[static_cast<uint8_t>(row.kind)]),
             static_cast<unsigned>(row.percentage));
    item.value = self.rowValue;
    return;
  }
  item.label = self.pageStart > 0 && rowIndex == self.count ? tr(STR_READING_INBOX_PREVIOUS)
                                                         : tr(STR_READING_INBOX_NEXT);
}

void ReadingInboxActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (!loaded || total == 0) {
    screen.centeredText(loaded ? tr(STR_READING_INBOX_EMPTY) : tr(STR_ANNOTATION_LOAD_FAILED), screen.theme().bodyText);
    screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  }
  fui::ListProps props;
  props.rowProvider = &ReadingInboxActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void ReadingInboxActivity::activateIndex(const int index) {
  if (popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (index == 0) {
    RenderLock lock;
    pageStart = 0;
    reload();
    requestUpdate();
  } else if (index == 1) {
    exportInbox();
  } else if (static_cast<size_t>(index - 2) < count) {
    openEntry(static_cast<size_t>(index - 2));
  } else {
    RenderLock lock;
    if (pageStart > 0 && static_cast<size_t>(index - 2) == count) pageStart -= PAGE_SIZE;
    else pageStart += PAGE_SIZE;
    reload();
    requestUpdate();
  }
}

void ReadingInboxActivity::openEntry(const size_t index) {
  auto entry = makeUniqueNoThrow<AnnotationEntry>();
  if (!entry) {
    LOG_ERR("INBOX", "OOM: annotation entry");
    showMessage(StrId::STR_ANNOTATION_LOAD_FAILED);
    return;
  }
  const auto& row = rows[index];
  if (AnnotationStore::loadFromSourceKey(row.sourceKey, row.id, *entry) != AnnotationStore::Result::Success) {
    showMessage(StrId::STR_ANNOTATION_LOAD_FAILED);
    return;
  }
  if (!Storage.exists(entry->sourcePath.c_str())) {
    showMessage(StrId::STR_READING_INBOX_SOURCE_MISSING, entry->sourcePath.c_str());
    return;
  }
  auto reader = ReaderActivity::create(renderer, mappedInput, entry->sourcePath, false);
  if (!reader || !reader->openAnnotation(*entry)) {
    showMessage(StrId::STR_READING_INBOX_OPEN_FAILED, entry->sourcePath.c_str());
    return;
  }
  activityManager.replaceActivity(std::move(reader));
}

void ReadingInboxActivity::exportInbox() {
  std::string outputPath;
  if (!AnnotationExport::writeInboxMarkdown(outputPath)) {
    showMessage(StrId::STR_ANNOTATION_EXPORT_FAILED);
    return;
  }
  showMessage(StrId::STR_ANNOTATION_EXPORTED, outputPath.c_str());
}

void ReadingInboxActivity::onBackButton() { activityManager.goHome(HomeMenuItem::READING_INBOX); }

void ReadingInboxActivity::showMessage(const StrId message, const char* detail) {
  RenderLock lock;
  const char* options[] = {tr(STR_OK_BUTTON)};
  popup.show(I18N.get(message), detail, options, 1, 0, [](int) {});
  requestUpdate();
}

bool ReadingInboxActivity::handleCustomInput() {
  return popup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void ReadingInboxActivity::render(RenderLock&& lock) {
  if (popup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
