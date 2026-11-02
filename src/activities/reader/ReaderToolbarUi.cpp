#include "ReaderToolbarUi.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/readerToolbarIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_DISMISS = 1;  // tap anywhere on the page above the sheet
constexpr fui::ActionId ACTION_TOOL = 2;     // value = 0 Contents, 1 Text, 2 More
constexpr fui::ActionId ACTION_PREV = 3;     // scrub row: previous chapter
constexpr fui::ActionId ACTION_NEXT = 4;     // scrub row: next chapter
constexpr fui::ActionId ACTION_SCRUB = 5;    // progress track: dragPermille along the book
constexpr fui::ActionId ACTION_ROW = 6;      // panel list row, value = row index

// The visible progress track stays light; every touch band is at least 48px.
constexpr int16_t kScrubButton = 48;
constexpr int16_t kScrubKnob = 16;
constexpr int16_t kScrubGap = 12;
constexpr int16_t kToolInset = 8;
constexpr int kToolCount = 3;
// Bottom sheet height for the panels. ListNav fits whole rows in the remaining
// list area; any spare pixels stay between the list and the switcher.
constexpr int kPanelHeightPercent = 62;
// Cap the sheet may grow to when rounding the list area up to a whole row.
constexpr int kPanelHeightMaxPercent = 72;
// Landscape has less vertical room; leave a narrow page strip for tap-to-dismiss.
constexpr int kLandscapePanelHeightPercent = 88;
}  // namespace

ReaderToolbarUi::ReaderToolbarUi(GfxRenderer& renderer) : UiAppHost(renderer), renderer_(renderer) {}

void ReaderToolbarUi::begin() {
  resetUi();
  pending_ = Routed{};
  nav_.reset();
  for (fui::ActionId id = ACTION_DISMISS; id <= ACTION_ROW; ++id) {
    app.on(id, &ReaderToolbarUi::onAction, this);
  }
  app.setScreen(&ReaderToolbarUi::screenFn, this);
}

void ReaderToolbarUi::render() {
  renderUi();
  // Wrapped rows can fit fewer than the fixed-height estimate; the nav then
  // advances the viewport after layout and asks for a rebuild. Converges (top
  // only moves forward toward the selection); the bound is a backstop.
  for (int pass = 0; pass < 3 && nav_.consumeRebuildNeeded(); ++pass) renderUi();
}

ReaderToolbarUi::Routed ReaderToolbarUi::route(const MappedInputManager& input) {
  pending_ = Routed{};
  // routeHeld: the scrub track is a drag target, so held frames must reach it.
  const auto touch = routeTouch(input, false, /*routeHeld=*/true);
  pending_.routed = touch.routed;
  pending_.x = touch.snap.touchX;
  pending_.y = touch.snap.touchY;
  // Only the release commits a scrub: every held frame dispatches too
  // (dragPermille set), and re-paginating a chapter per frame would be seconds
  // of work per swipe.
  if (pending_.event == Event::Scrub && !touch.snap.touchReleased) pending_.event = Event::None;
  return pending_;
}

void ReaderToolbarUi::onAction(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ReaderToolbarUi*>(user);
  Routed& out = self->pending_;
  out.value = event.value;
  out.permille = event.dragPermille;
  if (event.action >= ACTION_DISMISS && event.action <= ACTION_ROW) out.event = static_cast<Event>(event.action);
  if (out.event == Event::Scrub && event.dragPermille < 0) out.event = Event::None;
  // A handled action repaints through the reader's own fast path, not through
  // app.invalidate(): the page underneath is the reader's to draw.
  self->app.clearTapFlash();
}

void ReaderToolbarUi::screenFn(UiScreen& screen, void* user) {
  auto* self = static_cast<ReaderToolbarUi*>(user);
  if (self->model_.panel) {
    self->buildPanel(screen);
  } else {
    self->buildToolbar(screen);
  }
}

std::string ReaderToolbarUi::rowValue(const int index) const {
  if (model_.rowValue) return model_.rowValue(index);
  return index == model_.currentRow ? std::string(tr(STR_CURRENT_CHAPTER)) : std::string();
}

int16_t ReaderToolbarUi::toolRowHeight(UiScreen& screen) const {
  const auto& tokens = screen.theme();
  const fui::Rect safe = screen.frame().safeRect();
  const int16_t lineH = screen.target().lineHeight(tokens.smallText.font);
  return safe.width > safe.height ? std::max<int16_t>(48, lineH + 2 * tokens.spaceSm)
                                 : static_cast<int16_t>(24 + 2 * lineH + 3 * tokens.spaceSm);
}

// Paint and touch use the same full slots, including the final remainder.
void ReaderToolbarUi::buildToolRow(UiScreen& screen, const fui::LayoutAnchor anchor, const int16_t sideInset) {
  const auto& tokens = screen.theme();
  const fui::BitmapRef icons[kToolCount] = {fui::bitmapFromIcon(icon_reader_contents_24),
                                            fui::bitmapFromIcon(icon_reader_text_24),
                                            fui::bitmapFromIcon(icon_reader_more_24)};
  constexpr StrId labels[kToolCount] = {StrId::STR_TOOL_CONTENTS, StrId::STR_TOOL_TEXT, StrId::STR_TOOL_MORE};
  const bool landscape = screen.frame().safeRect().width > screen.frame().safeRect().height;
  fui::TextStyle labelStyle = tokens.smallText;
  labelStyle.align = fui::TextAlign::Center;
  labelStyle.maxLines = landscape ? 1 : 2;
  const fui::Rect row = screen.take(anchor, toolRowHeight(screen)).inset(fui::Insets{0, sideInset, 0, sideInset});
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  screen.target().fill(fui::Rect{row.x, row.y, row.width, 1}, ink);
  for (int i = 0; i < kToolCount; ++i) {
    const int column = UITheme::isRtl() ? kToolCount - 1 - i : i;
    const int16_t left = static_cast<int16_t>(row.x + row.width * column / kToolCount);
    const int16_t right = static_cast<int16_t>(row.x + row.width * (column + 1) / kToolCount);
    const fui::Rect slot{left, row.y, static_cast<int16_t>(right - left), row.height};
    if (i == model_.activeTool) {
      screen.target().fill(fui::Rect{static_cast<int16_t>(slot.x + kToolInset),
                                     static_cast<int16_t>(slot.bottom() - 2),
                                     static_cast<int16_t>(slot.width - 2 * kToolInset), 2}, ink);
    }
    fui::Rect textRect = slot.inset(fui::Insets{tokens.spaceSm, kToolInset, tokens.spaceSm, kToolInset});
    fui::Rect iconRect{static_cast<int16_t>(slot.x + (slot.width - 24) / 2),
                       static_cast<int16_t>(slot.y + tokens.spaceSm), 24, 24};
    if (landscape) {
      iconRect.x = UITheme::isRtl() ? static_cast<int16_t>(textRect.right() - 24) : textRect.x;
      iconRect.y = static_cast<int16_t>(slot.y + (slot.height - 24) / 2);
      textRect.width = static_cast<int16_t>(textRect.width - 24 - tokens.spaceSm);
      if (!UITheme::isRtl()) textRect.x = static_cast<int16_t>(textRect.x + 24 + tokens.spaceSm);
    } else {
      textRect.y = static_cast<int16_t>(iconRect.bottom() + tokens.spaceSm);
      textRect.height = static_cast<int16_t>(slot.bottom() - tokens.spaceSm - textRect.y);
    }
    screen.target().bitmap(iconRect, icons[i], fui::BitmapMode::Center);
    screen.target().text(textRect, I18N.get(labels[i]), labelStyle);
    screen.frame().hit(slot, ACTION_TOOL, static_cast<int16_t>(i), fui::InputTouch);
  }
}

void ReaderToolbarUi::buildToolbar(UiScreen& screen) {
  const auto& tokens = screen.theme();

  // Sheet height from its content: scrub row, meta line, tool row, and the
  // air between them (a full spaceLg under the scrub row, so the progress
  // track and its chapter buttons don't crowd the meta line).
  const int16_t metaH = screen.target().lineHeight(tokens.smallText.font);
  const int16_t contentH = static_cast<int16_t>(tokens.spaceMd + kScrubButton + tokens.spaceLg + metaH +
                                                tokens.spaceSm + toolRowHeight(screen) + tokens.spaceSm);
  fui::SheetProps sheetProps;
  sheetProps.anchor = fui::SheetEdge::Bottom;
  sheetProps.dismissAction = ACTION_DISMISS;
  sheetProps.ruleWidth = 1;
  sheetProps.grabberHeight = 2;
  sheetProps.grabberMargin = tokens.spaceMd;
  sheetProps.grabberInset = tokens.spaceMd;
  const int16_t grabberBand =
      static_cast<int16_t>(sheetProps.grabberMargin + sheetProps.grabberHeight + sheetProps.grabberInset);
  screen.sheet(sheetProps, static_cast<int16_t>(contentH + grabberBand));
  screen.insetContent(fui::Insets{0, tokens.headerSidePadding, 0, tokens.headerSidePadding});
  screen.spacer(tokens.spaceMd);

  // Scrub row: < [progress track + knob: tap/drag to jump] >
  {
    const fui::Rect band = screen.takeTop(kScrubButton, tokens.spaceLg);
    stepProps_.label = nullptr;
    stepProps_.icon = fui::bitmapFromIcon(icon_reader_back_24);
    stepProps_.action = ACTION_PREV;
    stepProps_.inputMask = fui::InputTouch;
    stepProps_.styles = tokens.button;
    screen.button(stepProps_, fui::Rect{band.x, band.y, kScrubButton, kScrubButton});
    stepProps_.icon = fui::bitmapFromIcon(icon_reader_next_24);
    stepProps_.action = ACTION_NEXT;
    screen.button(stepProps_,
                  fui::Rect{static_cast<int16_t>(band.right() - kScrubButton), band.y, kScrubButton, kScrubButton});

    // The full-height drag band shares its horizontal endpoints with the
    // thin track; the square handle marks book position without a fill slab.
    const int16_t trackX = static_cast<int16_t>(band.x + kScrubButton + kScrubGap);
    const int16_t trackW = static_cast<int16_t>(band.width - 2 * (kScrubButton + kScrubGap));
    const int16_t trackY = static_cast<int16_t>(band.y + band.height / 2);
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    screen.target().fill(fui::Rect{trackX, static_cast<int16_t>(trackY - 1), trackW, 2}, ink);
    const int permille = std::clamp(model_.progressPermille, 0, 1000);
    const int16_t knobX =
        static_cast<int16_t>(trackX + (static_cast<int32_t>(trackW - kScrubKnob) * permille) / 1000);
    screen.target().fill(fui::Rect{knobX, static_cast<int16_t>(trackY - kScrubKnob / 2),
                                   kScrubKnob, kScrubKnob},
                         ink, tokens.controlRadius);
    screen.frame().hit(fui::Rect{trackX, band.y, trackW, band.height}, ACTION_SCRUB, 0,
                       fui::InputTouch | fui::InputDrag);
  }

  // Chapter title and progress have separate, mirrored lanes.
  {
    const fui::Rect line = screen.takeTop(metaH, tokens.spaceSm);
    fui::TextStyle titleStyle = tokens.smallText;
    titleStyle.align = UITheme::isRtl() ? fui::TextAlign::Right : fui::TextAlign::Left;
    fui::TextStyle infoStyle = tokens.smallText;
    infoStyle.align = UITheme::isRtl() ? fui::TextAlign::Left : fui::TextAlign::Right;
    const int16_t infoW = model_.pageInfo
        ? std::min<int16_t>(line.width / 2, screen.target().measureText(infoStyle.font, model_.pageInfo, infoStyle).width)
        : 0;
    const int16_t titleW = std::max<int16_t>(0, line.width - infoW - tokens.spaceMd);
    const fui::Rect titleRect{UITheme::isRtl() ? static_cast<int16_t>(line.right() - titleW) : line.x,
                              line.y, titleW, line.height};
    const fui::Rect infoRect{UITheme::isRtl() ? line.x : static_cast<int16_t>(line.right() - infoW),
                             line.y, infoW, line.height};
    if (model_.chapterTitle) screen.target().text(titleRect, model_.chapterTitle, titleStyle);
    if (model_.pageInfo) screen.target().text(infoRect, model_.pageInfo, infoStyle);
  }

  buildToolRow(screen, fui::LayoutAnchor::Top, 0);
}

void ReaderToolbarUi::buildPanel(UiScreen& screen) {
  const auto& tokens = screen.theme();
  const fui::Rect safe = screen.frame().safeRect();

  fui::SheetProps sheetProps;
  sheetProps.anchor = fui::SheetEdge::Bottom;
  sheetProps.dismissAction = ACTION_DISMISS;  // tap the page above the sheet = back to the toolbar
  sheetProps.ruleWidth = 1;
  sheetProps.grabberHeight = 2;
  sheetProps.grabberMargin = tokens.spaceMd;
  sheetProps.grabberInset = tokens.spaceMd;

  // Size the sheet to whole list rows instead of a fixed screen share: a
  // percentage leaves up to a row's height of dead space above the switcher,
  // and a short list (Text, More) leaves empty row slots. Chrome = everything
  // in the sheet that is not the list; the row count starts from the target
  // share and grows one row when that still fits the cap. A list that fits
  // shrinks to its rows' measured (wrapped) heights.
  uiTarget.setFont(fui::GfxRendererTarget::FONT_TITLE, GUI.titleFontId(renderer_, model_.panelTitle));
  fui::TextStyle titleStyle = tokens.titleText;
  titleStyle.bold = false;
  titleStyle.align = UITheme::isRtl() ? fui::TextAlign::Right : fui::TextAlign::Left;
  titleStyle.maxLines = 2;
  const int16_t indicatorW = screen.target().measureText(tokens.smallText.font, "999/999", tokens.smallText).width;
  const int16_t titleW = static_cast<int16_t>(safe.width - 2 * tokens.headerSidePadding - indicatorW - tokens.spaceMd);
  const int16_t titleH = std::max(screen.target().lineHeight(titleStyle.font),
                                 fui::measureWrappedText(screen.target(), model_.panelTitle, titleStyle, titleW).height);
  const int16_t rowH = std::max<int16_t>(
      48, model_.denseRows ? static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight) : tokens.rowHeight);
  const int16_t rowGap = model_.denseRows ? tokens.listRowGap : std::max(tokens.listRowGap, tokens.listTouchRowGap);
  const int16_t rowStride = static_cast<int16_t>(rowH + rowGap);
  const int16_t grabberBand =
      static_cast<int16_t>(sheetProps.grabberMargin + sheetProps.grabberHeight + sheetProps.grabberInset);
  const int16_t chrome =
      static_cast<int16_t>(grabberBand + titleH + tokens.spaceMd + tokens.spaceSm + toolRowHeight(screen) + tokens.spaceSm);
  const bool landscape = safe.width > safe.height;
  const int16_t target =
      static_cast<int16_t>((safe.height * (landscape ? kLandscapePanelHeightPercent : kPanelHeightPercent)) / 100);
  const int16_t cap =
      static_cast<int16_t>((safe.height * (landscape ? kLandscapePanelHeightPercent : kPanelHeightMaxPercent)) / 100);
  int sheetRows = (target - chrome + rowGap) / rowStride;
  if (static_cast<int16_t>(chrome + (sheetRows + 1) * rowStride - rowGap) <= cap) ++sheetRows;
  const int count = std::max(0, model_.itemCount);
  if (count > 0 && sheetRows > count) sheetRows = count;
  if (sheetRows < 1) sheetRows = 1;

  listProps_.count = static_cast<uint16_t>(count);
  listProps_.action = ACTION_ROW;
  listProps_.inputMask = fui::InputTouch;  // physical buttons stay with the reader
  listProps_.rowHeight = rowH;
  listProps_.rowGap = rowGap;
  listProps_.labelText = tokens.bodyText;
  listProps_.labelText.maxLines = 2;
  listProps_.valueText = tokens.smallText;
  listProps_.sidePadding = 0;
  listProps_.rowInset = tokens.headerSidePadding;

  // A two-line label grows its row by one line. The band always holds one
  // such row; a row taller than the band could never be drawn or reached.
  const int16_t labelLh = screen.target().lineHeight(listProps_.labelText.font);
  const int16_t tallestRow = static_cast<int16_t>(2 * labelLh > rowH ? rowH + labelLh : rowH);
  int16_t listH = std::max<int16_t>(static_cast<int16_t>(sheetRows * rowStride - rowGap), tallestRow);
  // Rows materialised into window slots 0.. while sizing; the window reuses them.
  int sizedRows = 0;
  if (count > 0 && count == sheetRows && count <= kMaxWindow) {
    // Measure at list()'s row width: band minus row insets and the scroll
    // strip a nav-managed list always reserves.
    const int16_t strip = listProps_.scrollIndicator
                              ? static_cast<int16_t>(std::max(
                                    0, tokens.listScrollWidth + tokens.listScrollInset + 2 - listProps_.rowInset))
                              : 0;
    const int16_t rowW = static_cast<int16_t>(safe.width - 2 * listProps_.rowInset - strip);
    const int16_t room = std::max<int16_t>(static_cast<int16_t>(cap - chrome), tallestRow);
    int16_t fitted = 0;
    bool clipped = false;
    for (int i = 0; i < count; ++i) {
      windowLabels_[i] = model_.rowText ? model_.rowText(i) : std::string();
      windowValues_[i] = rowValue(i);
      fui::ListItem item;
      item.label = windowLabels_[i].c_str();
      item.value = windowValues_[i].empty() ? nullptr : windowValues_[i].c_str();
      const int16_t next = static_cast<int16_t>(
          fitted + (i > 0 ? rowGap : 0) + fui::measureListRow(screen.target(), nullptr, rowW, listProps_, item).height);
      if (next > room) clipped = true;
      if (!clipped) fitted = next;
    }
    sizedRows = count;
    // Rows past the cap scroll; the band still holds at least the first row.
    listH = clipped ? std::max(fitted, tallestRow) : fitted;
  }
  screen.sheet(sheetProps, static_cast<int16_t>(chrome + listH));
  // No blanket side inset: Screen::list() draws in the content band, and the
  // scroll track must reach the sheet's edge like a full-screen list's does.
  // The title insets itself; the rows inset via rowInset below.

  // Title line: panel name left, page position right when the list spans pages.
  {
    const fui::Rect line = screen.takeTop(titleH, tokens.spaceMd).inset(
        fui::Insets{0, tokens.headerSidePadding, 0, tokens.headerSidePadding});
    const fui::Rect titleRect{UITheme::isRtl() ? static_cast<int16_t>(line.right() - titleW) : line.x,
                              line.y, titleW, line.height};
    screen.target().text(titleRect, model_.panelTitle, titleStyle);
    pageIndicatorRect_ = fui::Rect{UITheme::isRtl() ? line.x : static_cast<int16_t>(line.right() - indicatorW),
                                   line.y, indicatorW, line.height};
  }

  // Switcher row along the sheet's bottom edge; the list takes what is left.
  screen.spacer(tokens.spaceSm, fui::LayoutAnchor::Bottom);
  buildToolRow(screen, fui::LayoutAnchor::Bottom, tokens.headerSidePadding);
  screen.spacer(tokens.spaceSm, fui::LayoutAnchor::Bottom);

  const fui::Rect listRect = screen.body();
  // The nav owns selection + viewport (same fui::ListNav idiom as the list
  // menu screens). A shown cursor re-follows into view on every build; a
  // hidden one (-1, touch) leaves the viewport where scrolling put it.
  nav_.selected = std::clamp(model_.selectedIndex, -1, count - 1);
  nav_.followOnBuild = nav_.selected >= 0;
  nav_.followPending = false;
  nav_.syncToProps(listRect, listProps_.rowHeight, rowGap, count, listProps_);

  // Materialise only the visible window of rows.
  const int windowCount = std::min({nav_.visibleRows, count - nav_.top, kMaxWindow});
  for (int i = 0; i < windowCount; ++i) {
    const int index = nav_.top + i;
    if (index != i || i >= sizedRows) {
      windowLabels_[i] = model_.rowText ? model_.rowText(index) : std::string();
      windowValues_[i] = rowValue(index);
    }
    fui::ListItem item;
    item.label = windowLabels_[i].c_str();
    item.value = windowValues_[i].empty() ? nullptr : windowValues_[i].c_str();
    item.actionValue = static_cast<int16_t>(index);
    windowItems_[i] = item;
  }
  listProps_.items = windowItems_;
  listProps_.itemsWindowFirst = static_cast<uint16_t>(nav_.top);
  listProps_.itemsWindowCount = static_cast<uint16_t>(std::max(0, windowCount));
  if (count > 0) {
    screen.list(listProps_);
  }

  const int pageRows = nav_.pageRows();
  const int totalPages = pageRows > 0 ? (count + pageRows - 1) / pageRows : 0;
  if (totalPages > 1) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d/%d", nav_.top / pageRows + 1, totalPages);
    fui::TextStyle pageStyle = tokens.smallText;
    pageStyle.align = UITheme::isRtl() ? fui::TextAlign::Left : fui::TextAlign::Right;
    screen.target().text(pageIndicatorRect_, buf, pageStyle);
  }
}
