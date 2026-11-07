#include "EndOfBookOptions.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "ReaderUtils.h"
// ReaderUtils.h pulls in ActivityManager.h, which only forward-declares Activity while holding
// std::unique_ptr<Activity> members. Destroying that unique_ptr needs the complete type, so the
// definition must be visible here.
#include "activities/Activity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/NextBookFinder.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;

// Display name without the file extension, mirroring the file browser rows
std::string displayName(const std::string& filename) {
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}
}  // namespace

EndOfBookOptions::EndOfBookOptions(GfxRenderer& renderer) : UiAppHost(renderer), renderer(renderer) {}

void EndOfBookOptions::loadOnce(const std::string& currentBookPath, const std::string& backToTitle) {
  if (isLoaded.load(std::memory_order_acquire)) {
    return;
  }
  folder = FsHelpers::extractFolderPath(currentBookPath);
  names = NextBookFinder::findNextBooks(currentBookPath, MAX_SUGGESTIONS);
  hasBackRow = !backToTitle.empty();
  if (hasBackRow) {
    backToLabel = std::string(tr(STR_BACK_TO)) + " " + backToTitle;
  }
  selector.store(0, std::memory_order_relaxed);
  // One-time app setup on the render task, including the Home-only case.
  resetUi();
  app.on(ACTION_ROW, &EndOfBookOptions::onRowEvent, this);
  app.setScreen(&EndOfBookOptions::listScreen, this);
  buildRowItems();
  // Release-publish so the main task, which gates all access on isLoaded, never
  // observes a partially built list (rowItems/rowLabels included)
  isLoaded.store(true, std::memory_order_release);
}

// Populates rowLabels/rowItems from names + the trailing "Home" row. Called
// once here since names never changes after loadOnce() completes.
void EndOfBookOptions::buildRowItems() {
  rowCount = 0;
  if (hasBackRow && rowCount < MAX_ROWS) {
    rowLabels[rowCount] = backToLabel;
    fui::ListItem item;
    item.label = rowLabels[rowCount].c_str();
    item.actionValue = static_cast<int16_t>(rowCount);
    rowItems[rowCount] = item;
    rowCount++;
  }
  for (const auto& name : names) {
    if (rowCount >= MAX_ROWS) break;
    rowLabels[rowCount] = displayName(name);
    fui::ListItem item;
    item.label = rowLabels[rowCount].c_str();
    const auto extension = name.rfind('.');
    item.value = extension == std::string::npos ? nullptr : name.c_str() + extension;
    item.subtitle = folder.c_str();
    item.actionValue = static_cast<int16_t>(rowCount);
    rowItems[rowCount] = item;
    rowCount++;
  }
  if (rowCount < MAX_ROWS) {
    rowLabels[rowCount] = tr(STR_EOB_HOME);
    fui::ListItem item;
    item.label = rowLabels[rowCount].c_str();
    item.actionValue = static_cast<int16_t>(rowCount);
    rowItems[rowCount] = item;
    rowCount++;
  }
}

bool EndOfBookOptions::menuActive() const { return isLoaded.load(std::memory_order_acquire); }

std::string EndOfBookOptions::fullPath(const size_t index) const {
  if (index >= names.size()) {
    return {};
  }
  return folder == "/" ? "/" + names[index] : folder + "/" + names[index];
}

void EndOfBookOptions::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EndOfBookOptions*>(user);
  if (event.value < 0 || static_cast<size_t>(event.value) >= self->rowCount) return;
  self->selector.store(event.value, std::memory_order_relaxed);
  // The tapped row leaves this screen (open book or home); a lingering flash
  // would gray an unrelated element on the next render.
  self->app.clearTapFlash();
  self->tappedRow = event.value;
}

// Row index -> the action that row performs. Rows are [Back to ...] [suggestions...]
// [Home]; the leading Quick Break row shifts the suggestions by one.
EndOfBookOptions::Action EndOfBookOptions::actionForRow(const int row, std::string* openPath) const {
  if (row < 0) {
    return Action::None;
  }
  if (hasBackRow && row == 0) {
    return Action::BackToBook;
  }
  const int nameIndex = row - nameRowOffset();
  if (nameIndex >= 0 && nameIndex < static_cast<int>(names.size())) {
    if (openPath) {
      *openPath = fullPath(static_cast<size_t>(nameIndex));
    }
    return Action::OpenBook;
  }
  return Action::GoHome;  // "Home" row
}

EndOfBookOptions::Action EndOfBookOptions::handleMenuInput(const MappedInputManager& input, std::string* openPath) {
  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent record the tapped row.
  tappedRow = -1;
  const auto route = routeTouch(input);
  // cppcheck can't see that route() dispatches into onRowEvent (registered via
  // app.on(ACTION_ROW, ...)), which sets tappedRow, so it flags this as always false.
  // cppcheck-suppress knownConditionTrueFalse
  if (route && tappedRow >= 0) {
    return actionForRow(tappedRow, openPath);
  }

  // Wrapped titles and folder lines can outgrow a landscape page. Vertical
  // swipes page the viewport by the rows the last build drew (the selection
  // stays put; buttons pull the view back to it), as on every list screen.
  // Edge swipes keep their reader/home meanings, and a list that fit
  // entirely gets no repaint.
  if (!names.empty() && nav.inputPageRows() < static_cast<int>(rowCount) && !input.wasMenuGesture() &&
      !input.wasHomeGesture() && !input.wasReaderMenuSwipeUp()) {
    const auto swipe = input.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int rows = nav.inputPageRows();
      nav.requestScroll(swipe == MappedInputManager::SwipeDir::Up ? rows : -rows);
      return Action::Redraw;
    }
  }

  if (route.routed && app.invalidated()) {
    return Action::Redraw;
  }

  const int selectedIndex = selector.load(std::memory_order_relaxed);
  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    return actionForRow(selectedIndex, openPath);
  }

  // Short-press Back returns to the last page; a long press falls through to the
  // reader's own handler (file browser). Home is reached through the list's Home entry.
  if (input.wasReleased(MappedInputManager::Button::Back) && input.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    return Action::LastPage;
  }

  // Selection movement on the standard list navigation buttons (side Up/Down plus front
  // Left/Right, orientation swap included). It follows the reader's page-turn semantics
  // (press-triggered by default, release-triggered when a long-press behavior is
  // configured, same rule as ReaderUtils::detectPageTurn). This matters on entry: with
  // press-triggered turns, the press that turned the final page already fired in the
  // reader, and its release must not double-fire into this menu.
  const bool usePress = SETTINGS.longPressButtonBehavior == CrossPointSettings::OFF;
  const auto triggered = [&](const MappedInputManager::Button button) {
    return usePress ? input.wasPressed(button) : input.wasReleased(button);
  };
  const int itemCount = nameRowOffset() + static_cast<int>(names.size()) + 1;  // + "Home" entry
  if (triggered(MappedInputManager::Button::NavPrevious)) {
    if (itemCount <= 1) return Action::LastPage;
    selector.store(ButtonNavigator::previousIndex(selectedIndex, itemCount), std::memory_order_relaxed);
    return Action::Redraw;
  }
  if (triggered(MappedInputManager::Button::NavNext)) {
    if (itemCount <= 1) return Action::GoHome;
    selector.store(ButtonNavigator::nextIndex(selectedIndex, itemCount), std::memory_order_relaxed);
    return Action::Redraw;
  }
  return Action::None;
}

void EndOfBookOptions::listScreen(UiScreen& screen, void* user) {
  static_cast<EndOfBookOptions*>(user)->buildListScreen(screen);
}

void EndOfBookOptions::buildListScreen(UiScreen& screen) {
  const Rect body = UITheme::getContentBounds(renderer);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(body.y), static_cast<int16_t>(renderer.getScreenWidth() - (body.x + body.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (body.y + body.height)), static_cast<int16_t>(body.x)});
  if (!names.empty()) {
    auto heading = screen.theme().smallText;
    heading.align = UITheme::isRtl() ? fui::TextAlign::Right : fui::TextAlign::Left;
    heading.maxLines = 2;
    const int16_t height = fui::measureWrappedText(screen.target(), tr(STR_EOB_CONTINUE_WITH), heading,
                                                  screen.body().width).height;
    screen.target().text(screen.takeTop(height), tr(STR_EOB_CONTINUE_WITH), heading);
    screen.spacer(screen.theme().spaceMd);
  }

  // rowLabels/rowItems were built once in loadOnce() (see buildRowItems())
  // and reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(rowCount);
  const int selected = selector.load(std::memory_order_relaxed);
  if (nav.selected != selected) nav.followOnBuild = true;
  nav.selected = selected;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in handleMenuInput()
  props.rowInset = 0;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.subtitleText = screen.theme().smallText;
  props.valueText = screen.theme().smallText;
  props.balanceWrappedLabelWithValue = false;
  screen.syncListViewport(nav, props, static_cast<int>(rowCount));
  screen.list(props);
}

void EndOfBookOptions::render(GfxRenderer& renderer, const MappedInputManager& input) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight};
  GUI.drawHeader(renderer, header, tr(STR_END_OF_BOOK));

  // The list renders through the FreeInkApp so its rows register touch hit
  // rects; renderUi re-derives the device context, picking up any rotation
  // since construction (reader menu rotate).
  renderUi();
  for (int pass = 0; pass < static_cast<int>(MAX_ROWS) && nav.consumeRebuildNeeded(); ++pass) {
    renderer.clearScreen();
    GUI.drawHeader(renderer, header, tr(STR_END_OF_BOOK));
    renderUi();
  }

  const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
