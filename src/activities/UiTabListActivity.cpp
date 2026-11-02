#include "UiTabListActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cassert>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr int16_t TOUCH_TAB_BAR_HEIGHT = 48;
}

UiTabListActivity::UiTabListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const bool wantsTouchLongPress)
    : UiListActivity(name, renderer, mappedInput, wantsTouchLongPress) {}

void UiTabListActivity::onEnter() {
  // Size the per-tab state before the base resets activeNav() (which indexes
  // into it).
  tabNavs.assign(static_cast<size_t>(tabCount()), fui::ListNav{});
  UiListActivity::onEnter();
  app.on(ACTION_TAB, &UiTabListActivity::tabActionTrampoline, this);
}

fui::ListNav& UiTabListActivity::activeNav() {
  if (tabNavs.empty()) return nav;  // pre-onEnter fallback
  // Invariant: subclasses keep activeTab() inside [0, tabCount()), and
  // tabCount() does not change after onEnter() sized tabNavs.
  assert(activeTab() >= 0 && static_cast<size_t>(activeTab()) < tabNavs.size());
  return tabNavs[static_cast<size_t>(activeTab())];
}

int UiTabListActivity::ringPos() const {
  if (tabNavs.empty()) return 0;
  assert(activeTab() >= 0 && static_cast<size_t>(activeTab()) < tabNavs.size());
  return tabNavs[static_cast<size_t>(activeTab())].selected;
}

void UiTabListActivity::tabActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiTabListActivity*>(user);
  if (event.value < 0 || event.value >= self->tabCount()) return;
  self->onTabAction(event.value);
}

void UiTabListActivity::onRowAction(const fui::ActionEvent& event) {
  activeNav().selected = event.value + 1;  // ring position, not row index
  if (event.longPress) {
    onRowLongPress(event.value);
    return;
  }
  activateIndex(event.value);
}

void UiTabListActivity::moveRingTo(const int ringIndex) {
  activeNav().requestSelection(ringIndex);
  requestUpdate();
}

void UiTabListActivity::navigateButtons() {
  // Buttons walk the tab band (index 0) plus the rows (1..listCount).
  const int ringSize = listCount() + 1;
  buttonNavigator.onNextRelease([this, ringSize] { moveRingTo(ButtonNavigator::nextIndex(ringPos(), ringSize)); });
  buttonNavigator.onPreviousRelease(
      [this, ringSize] { moveRingTo(ButtonNavigator::previousIndex(ringPos(), ringSize)); });
  buttonNavigator.onNextContinuous([this] { stepTab(1); });
  buttonNavigator.onPreviousContinuous([this] { stepTab(-1); });
}

void UiTabListActivity::syncTabListViewport(UiScreen& screen, fui::ListProps& props) {
  syncListViewport(screen, props, 1);
}

void UiTabListActivity::buildTabBar(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  // The fixed tab set lives on the stack; render never allocates a container.
  constexpr int MAX_TABS = 8;
  const int count = tabCount() < MAX_TABS ? tabCount() : MAX_TABS;
  if (count <= 0) return;
  fui::TabItem tabs[MAX_TABS];
  for (int i = 0; i < count; i++) {
    tabs[i].label = tabLabel(i);
    tabs[i].value = static_cast<int16_t>(i);
    tabs[i].selected = activeTab() == i;
    tabs[i].indicator = tabIndicator(i);
  }
  fui::TabBarProps tabProps;
  tabProps.tabs = tabs;
  tabProps.count = static_cast<uint16_t>(count);
  tabProps.action = ACTION_TAB;
  tabProps.inputMask = fui::InputTouch;
  tabProps.minTouchSize = 48;
  tabProps.text = screen.theme().smallText;
  tabProps.gap = static_cast<int16_t>(metrics.tabSpacing);
  tabProps.tabInset = fui::Insets{2, 0, 0, 0};
  tabProps.contentInset = fui::Insets{2, 0, 4, 0};
  const int16_t tabLineHeight = screen.target().lineHeight(tabProps.text.font);
  const int16_t preferredTabHeight =
      mappedInput.hasTouch() ? TOUCH_TAB_BAR_HEIGHT : static_cast<int16_t>(metrics.tabBarHeight);
  const int16_t side = static_cast<int16_t>(metrics.contentSidePadding);
  const int availableWidth = screen.body().width - 2 * side;
  if (availableWidth <= 0) return;
  int widestLabel = 0;
  for (int i = 0; i < count; ++i) {
    const int indicatorWidth = tabs[i].indicator == fui::TabIndicator::None
                                   ? 0
                                   : tabProps.indicatorSize + tabProps.indicatorGap;
    widestLabel = std::max(widestLabel, screen.target().measureText(tabProps.text.font, tabs[i].label,
                                                                   tabProps.text).width + indicatorWidth);
  }
  // Preserve full category names: fold a crowded band into rows before
  // resorting to two text lines inside a tab.
  int columns = count;
  const int horizontalInset = tabProps.tabInset.left + tabProps.tabInset.right + 8;
  while (columns > 1 &&
         widestLabel + horizontalInset > (availableWidth - (columns - 1) * tabProps.gap) / columns) {
    --columns;
  }
  const int slotWidth = (availableWidth - (columns - 1) * tabProps.gap) / columns;
  tabProps.text.maxLines = widestLabel + horizontalInset > slotWidth ? 2 : 1;
  const int verticalInset =
      tabProps.tabInset.top + tabProps.tabInset.bottom + tabProps.contentInset.top + tabProps.contentInset.bottom;
  const int16_t tabBand = std::max<int16_t>(preferredTabHeight, tabLineHeight * tabProps.text.maxLines + verticalInset);
  const int rowCount = (count + columns - 1) / columns;

  // The underline identifies the current category even while focus is in the
  // list. A thin outline distinguishes keyboard focus on the tab band.
  fui::StyleSet tabStyles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
  if (ringPos() == 0) {
    tabStyles.selected.border = fui::Paint::solid(fui::Color::Black);
    tabStyles.selected.borderWidth = 1;
  }
  tabStyles.focused = tabStyles.selected;
  tabStyles.active = tabStyles.selected;
  tabProps.tabStyles = tabStyles;
  tabProps.selectedUnderline = 2;
  const fui::Rect tabRect = screen.takeTop(static_cast<int16_t>(tabBand * rowCount))
                                .inset(fui::Insets{0, side, 0, side});
  tabProps.divider = false;
  for (int row = 0; row < rowCount; ++row) {
    tabProps.tabs = tabs + row * columns;
    tabProps.count = static_cast<uint8_t>(std::min(columns, count - row * columns));
    if (UITheme::isRtl()) {
      std::reverse(tabs + row * columns, tabs + row * columns + tabProps.count);
    }
    const fui::Rect slotsRect{tabRect.x, static_cast<int16_t>(tabRect.y + row * tabBand),
                              tabRect.width, tabBand};
    fui::tabBar(screen.frame(), slotsRect, tabProps);
  }
  screen.target().fill(fui::Rect{tabRect.x, static_cast<int16_t>(tabRect.bottom() - 1), tabRect.width, 1},
                       fui::Paint::solid(fui::Color::Black));
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
}
