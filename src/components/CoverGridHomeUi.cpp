#include "CoverGridHomeUi.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "UITheme.h"
#include "util/BookProgress.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId SELECT = 1;
// Grid cell padding around each cover; also feeds the screen's horizontal
// inset so the cover columns land on the header chrome's inset line.
constexpr int16_t COVER_CELL_INSET = 6;
}  // namespace

CoverGridHomeUi::CoverGridHomeUi(GfxRenderer& renderer)
    : UiAppHost(renderer), coverCache(renderer), renderer(renderer) {}

void CoverGridHomeUi::begin(const std::vector<RecentBook>& recent, bool opds, bool continuing) {
  books = &recent;
  hasOpds = opds;
  hasContinueReading = continuing;
  if (!recent.empty()) coverCache.begin();
  resetUi();
  app.on(SELECT, &CoverGridHomeUi::onAction, this);
  app.setScreen(&CoverGridHomeUi::screenFn, this);
  refreshCoverPaths();
  progress = hasContinueReading && !books->empty() ? loadBookProgress(books->front().path) : -1;
  if (progress >= 0) snprintf(progressText, sizeof(progressText), "%d%%", progress);
}

void CoverGridHomeUi::refreshCoverPaths() {
  coverCache.invalidate();
  for (size_t i = 0; i < books->size() && i < coverPaths.size(); ++i) refreshCoverPath(i);
}

void CoverGridHomeUi::refreshCoverPath(size_t index) {
  if (index >= books->size() || index >= coverPaths.size()) return;
  coverCache.invalidate(index);
  coverPaths[index] =
      thumbHeight > 0 ? UITheme::getCoverThumbPath((*books)[index].coverBmpPath, thumbHeight) : std::string();
}

int CoverGridHomeUi::thumbHeightFor() const { return thumbHeight > 0 ? thumbHeight : THUMB_HEIGHT; }

bool CoverGridHomeUi::takeThumbHeightChanged() { return std::exchange(thumbHeightChanged, false); }

void CoverGridHomeUi::noteThumbHeight(int slotHeight) {
  // One shared height for every slot (hero and grid covers are the same
  // size). The +8 surplus gives the paint clip a little bleed so the art's
  // rightward nudge never exposes the left edge; covers narrower than the
  // 2:3 slot may show a thin sliver at the sides rather than being cropped.
  const int height = std::max(1, slotHeight + 8);
  if (thumbHeight != height) {
    thumbHeight = height;
    thumbHeightChanged = true;
    refreshCoverPaths();
  }
}

void CoverGridHomeUi::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<CoverGridHomeUi*>(user);
  self.pending = event.value;
  self.app.clearTapFlash();
}

int CoverGridHomeUi::selectedAction(const MappedInputManager& input) {
  pending = -1;
  const auto touch = routeTouch(input);
  return touch.snap.touchReleased ? pending : -1;
}

void CoverGridHomeUi::screenFn(UiScreen& screen, void* user) { static_cast<CoverGridHomeUi*>(user)->draw(screen); }

void CoverGridHomeUi::draw(UiScreen& screen) {
  coverCache.prepare();
  const auto& theme = screen.theme();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y), static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  // Outer cover columns align with the header chrome and route labels.
  const int16_t hInset = std::max<int16_t>(
      0, static_cast<int16_t>(UITheme::getInstance().getMetrics().headerSidePadding - COVER_CELL_INSET - safe.x));
  // Drop the status band, hero, and grid down: touch boards get a full step
  // (no button-hint band to fit), button boards a modest nudge. The tabs stay
  // put (bottom-anchored below).
  const int16_t topInset =
      BoardConfig::hasTouch() ? static_cast<int16_t>(theme.spaceLg) : static_cast<int16_t>(theme.spaceSm + 4);
  screen.insetContent(fui::Insets{topInset, hInset, theme.spaceSm, hInset});
  const int16_t navRowHeight = std::max<int16_t>(48, 2 * screen.target().lineHeight(theme.smallText.font) + 8);
  screen.takeTop(UITheme::getInstance().getMetrics().batteryBarHeight, theme.spaceSm);
  if (books->empty()) {
    drawEmpty(screen);
    screen.spacer(16);
    drawTabs(screen, screen.takeTop(static_cast<int16_t>(navRowHeight * 2)));
    drawHeaderBand();
    return;
  }
  auto tabRect = screen.takeBottom(static_cast<int16_t>(navRowHeight * 2), theme.spaceSm);
  // Hero cover matches the grid covers: the body splits into three equal
  // cover rows (hero + two grid rows), so the featured section is one row
  // tall and every cover on screen shares one size. The metadata floor still
  // applies when the rows would be too short for the hero's text lines.
  const fui::Rect body = screen.body();
  const int rowGap = std::max<int>(4, body.width / 100);
  grid.gap = grid.rowGap = rowGap;
  const int nominalRowHeight = std::max(1, (body.height - theme.spaceSm - 2 * rowGap) / 3);
  const int16_t featuredHeight = std::min<int>(
      body.height, std::max<int>(nominalRowHeight, screen.target().lineHeight(theme.bodyText.font) * 2 +
                                                     screen.target().lineHeight(theme.smallText.font) * 2 + 32));
  const int coverRowHeight = std::max(1, std::min(nominalRowHeight,
                                                (body.height - featuredHeight - theme.spaceSm - rowGap) / GRID_ROWS));
  drawCurrent(screen, screen.takeTop(featuredHeight, theme.spaceSm), coverRowHeight);
  drawGrid(screen);
  tabRect.x = gridBounds.x + grid.cellInset.left;
  tabRect.width = gridBounds.width - grid.cellInset.left - grid.cellInset.right;
  drawTabs(screen, tabRect);
  drawHeaderBand();
}

void CoverGridHomeUi::drawHeaderBand() {
  // The stock full-width band at the theme's topPadding, exactly like every
  // pushed screen's header: the battery/clock hold one position across the
  // whole UI, and the grid is widened to meet them (see the hInset above).
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.batteryBarHeight}, nullptr);
}

void CoverGridHomeUi::drawEmpty(UiScreen& screen) {
  const auto& theme = screen.theme();
  const auto body = screen.takeTop(144);
  auto title = theme.titleText;
  title.bold = false;
  title.maxLines = 2;
  auto message = theme.bodyText;
  message.maxLines = 2;
  const int16_t titleHeight = fui::measureWrappedText(screen.target(), tr(STR_NO_OPEN_BOOK), title, body.width).height;
  const int16_t messageHeight = fui::measureWrappedText(screen.target(), tr(STR_START_READING), message,
                                                       body.width).height;
  const int16_t y = body.y + std::max(0, (body.height - titleHeight - messageHeight - 8) / 2);
  screen.target().text(fui::Rect{body.x, y, body.width, titleHeight}, tr(STR_NO_OPEN_BOOK), title);
  screen.target().text(fui::Rect{body.x, static_cast<int16_t>(y + titleHeight + 8), body.width, messageHeight},
                       tr(STR_START_READING), message);
}

void CoverGridHomeUi::drawCurrent(UiScreen& screen, fui::Rect rect, const int coverRowHeight) {
  const auto& theme = screen.theme();
  const auto& book = books->front();
  card.title = book.title.c_str();
  card.author = nullptr;
  card.meta = nullptr;
  card.progressLabel = nullptr;
  card.centerTextOnCover = false;
  card.progressMax = 0;
  card.action = SELECT;
  // The featured card's selected state is a slim accent bar drawn after the
  // card (see below), not a bookCard indicator: every ring/outline treatment
  // tried here either overwhelmed the large cover or made the heading above
  // read as misaligned.
  card.state = fui::StateNormal;
  card.styles = theme.listRow;
  card.styles.selected = card.styles.normal;
  card.styles.selected.border = fui::Paint::solid(fui::Color::Black);
  card.styles.selected.radius = 0;
  card.styles.active = card.styles.selected;
  card.titleText = theme.bodyText;
  card.titleText.maxLines = 2;
  card.padding = fui::Insets{6, 6, 6, 6};
  card.gap = theme.spaceLg + theme.spaceSm;
  // Fit the shared 2:3 cover box within both the row and a grid column. No
  // gap reserve: the cell insets already keep adjacent covers apart, and the
  // SpaceBetween layout absorbs whatever slack remains.
  const int maxCoverWidth = rect.width / GRID_COLUMNS - 2 * COVER_CELL_INSET;
  card.coverSize.height = std::max(1, std::min(std::min<int>(rect.height, coverRowHeight) - 12, maxCoverWidth * 3 / 2));
  card.coverSize.width = std::max(1, card.coverSize.height * 2 / 3);
  noteThumbHeight(card.coverSize.height);
  gridBounds = layoutGrid(screen.body());
  rect.x = gridBounds.x;
  rect.width = gridBounds.width;
  card.coverPainterUserData = this;
  card.coverPainter = [](fui::DrawTarget& target, fui::Rect cover, const fui::BookCardProps&, void* user) {
    return static_cast<CoverGridHomeUi*>(user)->paintFramedCover(target, cover, 0);
  };
  fui::bookCard(screen.frame(), rect, card);
  const int16_t textX = rect.x + card.padding.left + card.coverSize.width + card.gap;
  const int16_t textWidth = std::max(0, rect.right() - card.padding.right - textX);
  if (!book.author.empty()) {
    const int16_t titleHeight = fui::measureWrappedText(screen.target(), book.title.c_str(), card.titleText,
                                                        textWidth).height;
    screen.target().text({textX, static_cast<int16_t>(rect.y + card.padding.top + titleHeight + 4), textWidth,
                          screen.target().lineHeight(theme.smallText.font)}, book.author.c_str(), theme.smallText);
  }
  if (progress >= 0) {
    const int16_t x = textX;
    const int16_t height = screen.target().lineHeight(theme.smallText.font);
    const int16_t y = rect.bottom() - card.padding.bottom - height;
    const int16_t labelWidth = screen.target().measureText(theme.smallText.font, progressText, theme.smallText).width;
    screen.target().text({x, y, labelWidth, height}, progressText, theme.smallText);
    const int16_t barX = x + labelWidth + 8;
    const int16_t barWidth = std::max(0, rect.right() - card.padding.right - barX);
    const fui::Rect track{barX, static_cast<int16_t>(y + height / 2 - 2), barWidth, 4};
    screen.target().stroke(track, fui::Paint::solid(fui::Color::Black), 1);
    screen.target().fill({track.x, track.y, static_cast<int16_t>(barWidth * std::clamp(progress, 0, 100) / 100), track.height},
                         fui::Paint::solid(fui::Color::Black));
  }

  if (selected == 0 && !BoardConfig::hasTouch()) {
    // Button boards only: a vertical accent bar left of the card, cover-height
    // and vertically centered on it, marks the featured card as the button
    // cursor without framing the cover. Touch boards tap directly and need no
    // cursor on the hero card.
    const int16_t barH = card.coverSize.height;
    screen.target().fill(
        fui::Rect{static_cast<int16_t>(rect.x - 5), static_cast<int16_t>(rect.y + (rect.height - barH) / 2), 3, barH},
        fui::Paint::solid(fui::Color::Black));
  }
}

fui::Rect CoverGridHomeUi::layoutGrid(fui::Rect rect) {
  // Exactly the hero's box: the three-equal-rows split in draw() already
  // guarantees it fits, and sharing the size keeps one cached thumb per book.
  grid.cellInset = fui::Insets{COVER_CELL_INSET, COVER_CELL_INSET, COVER_CELL_INSET, COVER_CELL_INSET};
  grid.coverSize = card.coverSize;
  grid.rowHeight = grid.coverSize.height + 12;
  // Full content width: the SpaceBetween column layout pins the outer covers
  // to the rect edges, so the grid reaches the chrome's inset line instead of
  // centering at its natural width.
  rect.height = GRID_ROWS * grid.rowHeight + (GRID_ROWS - 1) * grid.rowGap;
  return rect;
}

void CoverGridHomeUi::drawGrid(UiScreen& screen) {
  const auto rect = gridBounds;
  grid.count = books->size() > 1 ? books->size() - 1 : 0;
  grid.columns = GRID_COLUMNS;
  grid.columnLayout = fui::CoverGridColumnLayout::SpaceBetween;
  grid.action = SELECT;
  grid.inputMask = fui::InputTouch;
  grid.selectedIndex = selected > 0 && selected < static_cast<int>(books->size()) ? selected - 1 : -1;
  grid.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  grid.selectedCoverFrameGap = 2;
  grid.selectedCoverFrameWidth = 2;
  grid.cellStyles = card.styles;
  grid.labelHeight = 0;
  grid.labelGap = 0;
  grid.scrollIndicator = false;
  grid.itemProvider = [](uint16_t index, void*) { return fui::coverGridItem(nullptr, index + 1); };
  grid.coverPainterUserData = this;
  grid.coverPainter = [](fui::DrawTarget& target, fui::Rect cover, const fui::CoverGridItem&, uint16_t index,
                         void* user) {
    return static_cast<CoverGridHomeUi*>(user)->paintFramedCover(target, cover, index + 1);
  };
  fui::coverGrid(screen.frame(), rect, grid);
}

void CoverGridHomeUi::drawTabs(UiScreen& screen, fui::Rect rect) {
  static constexpr StrId LABELS[] = {StrId::STR_BROWSE_FILES, StrId::STR_LIBRARY, StrId::STR_READING_INBOX,
                                     StrId::STR_OPDS_BROWSER, StrId::STR_FILE_TRANSFER, StrId::STR_SETTINGS_TITLE};
  int count = 0;
  for (int i = 0; i < 6; ++i) {
    if (i == 3 && !hasOpds) continue;
    auto& tab = tabItems[count];
    tab.value = books->size() + count;
    tab.selected = selected == tab.value;
    tab.label = I18N.get(LABELS[i]);
    ++count;
  }
  tabs.tabs = tabItems.data();
  tabs.count = count;
  tabs.layout = fui::TabBarLayout::EqualWidth;
  tabs.action = SELECT;
  tabs.inputMask = fui::InputTouch;
  tabs.text = screen.theme().smallText;
  tabs.text.maxLines = 2;
  tabs.minTouchSize = 48;
  tabs.tabInset = fui::Insets{2, 4, 2, 4};
  tabs.contentInset = fui::Insets{2, 4, 2, 4};
  tabs.tabStyles.normal.background = fui::Paint::solid(fui::Color::White);
  tabs.tabStyles.selected.background = fui::Paint::solid(fui::Color::White);
  tabs.selectedUnderline = 2;
  tabs.distributedSlotWidth = 0;
  for (int row = 0; row < 2; ++row) {
    tabs.tabs = tabItems.data() + row * 3;
    tabs.count = std::min(3, count - row * 3);
    fui::tabBar(screen.frame(),
                {rect.x, static_cast<int16_t>(rect.y + row * rect.height / 2), rect.width,
                 static_cast<int16_t>(rect.height / 2)}, tabs);
  }
}

bool CoverGridHomeUi::paintFramedCover(fui::DrawTarget&, fui::Rect rect, size_t index) {
  if (index < coverPaths.size() && coverCache.paint(rect, index, coverPaths[index])) return true;
  GUI.drawCoverPlaceholder(renderer, Rect{rect.x, rect.y, rect.width, rect.height});
  return true;
}
