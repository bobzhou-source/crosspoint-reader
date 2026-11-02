#pragma once
#include <I18n.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

// Modal option picker drawn over the current screen (no clear) via
// fui::optionDialog. Touch hit-testing is the SDK's InteractionBuffer: each
// render registers the option buttons (plus a chrome guard rect) on the render
// task, and handleInput routes touch snapshots against that table on the loop
// task, gated by the uiReady handshake (same pattern as UiListActivity).
// render() builds into InteractionBuffer's non-published generation
// (beginPublishCycle()) and publishes it only once every hit() call for the
// frame is done (publish()), so handleInput()'s routePublished()/
// publishedData() reads on the loop task always see a complete table, never
// one render is mid-rebuilding. uiReady closes when show() replaces the
// popup's data, then stays open across ordinary repaints after the first
// publication so a release cannot be dropped during a highlight repaint.
class OptionPopup {
 public:
  void show(StrId titleId, const StrId* optionIds, int optionCount, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    headline.clear();
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = I18N.get(optionIds[i]);
    }
    activate(currentIndex, std::move(onSelect));
  }

  void show(const char* titleStr, const char* const* options, int optionCount, int currentIndex,
            std::function<void(int)> onSelect) {
    title = titleStr;
    headline.clear();
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = options[i];
    }
    activate(currentIndex, std::move(onSelect));
  }

  // As above, plus a subject line inside the dialog (a book or event title).
  // It wraps to several lines under the caption; the dialog grows to fit.
  void show(const char* titleStr, const char* headlineStr, const char* const* options, int optionCount,
            int currentIndex, std::function<void(int)> onSelect) {
    show(titleStr, options, optionCount, currentIndex, std::move(onSelect));
    headline = headlineStr ? headlineStr : "";
  }

  void show(StrId titleId, const std::vector<std::string>& options, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    headline.clear();
    ownedStrings = options;
    activate(currentIndex, std::move(onSelect));
  }

  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active) return false;

    const int count = std::min<int>(ownedStrings.size(), MAX_OPTIONS);
    const auto swipe = input.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int step = std::max(1, visibleOptions.load());
      selectedIndex = std::max(0, std::min(count - 1,
          selectedIndex + (swipe == MappedInputManager::SwipeDir::Up ? step : -step)));
      requestUpdate();
      return true;
    }
    const freeink::ui::InputSnapshot snap = touchSnapshotFrom(input);
    if (snap.touchPressed || snap.touchReleased || snap.touchHeld) {
      // Interactions are registered on the render task; only route once the
      // first render after show() has populated the table (uiReady handshake).
      if (uiReady) {
        const freeink::ui::ActionEvent event = interactions.routePublished(snap);
        if (event && event.action == ACTION_OPTION) {
          // Tap released on an option: select it, fire, dismiss.
          selectedIndex = event.value;
          active = false;
          auto callback = std::move(onSelectCallback);
          if (callback) callback(selectedIndex);
          requestUpdate();
          return true;
        }
        if (event && event.action == ACTION_CHROME) {
          // Taps on the dialog chrome (title, padding) keep the popup open.
          return true;
        }
        if (snap.touchReleased && snap.touchX >= 0) {
          // Tap released outside the dialog: dismiss without firing. Swipe-end
          // releases arrive with -1,-1 coords and fall through (no dismiss).
          active = false;
          requestUpdate();
          return true;
        }
        if (snap.touchPressed) {
          // Touch-down on an option moves the highlight (route() latched the
          // hit as the active interaction; read it back, no re-hit-testing).
          const int16_t idx = interactions.activeIndex();
          if (idx >= 0) {
            const freeink::ui::Interaction& hit = interactions.publishedData()[idx];
            if (hit.action == ACTION_OPTION && selectedIndex != hit.value) {
              selectedIndex = hit.value;
              requestUpdate();
            }
          }
        }
      }
      return true;
    }

    if (input.wasPressed(MappedInputManager::Button::NavPrevious)) {
      selectedIndex = (selectedIndex - 1 + count) % count;
      requestUpdate();
      return true;
    } else if (input.wasPressed(MappedInputManager::Button::NavNext)) {
      selectedIndex = (selectedIndex + 1) % count;
      requestUpdate();
      return true;
    } else if (input.wasReleased(MappedInputManager::Button::Confirm)) {
      active = false;
      auto callback = std::move(onSelectCallback);
      if (callback) callback(selectedIndex);
      requestUpdate();
      return true;
    } else if (input.wasReleased(MappedInputManager::Button::Back)) {
      active = false;
      requestUpdate();
      return true;
    }
    return true;
  }

  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    if (!active) {
      painted = false;
      return false;
    }
    const auto popupLabels = input.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, popupLabels.btn1, popupLabels.btn2, popupLabels.btn3, popupLabels.btn4);
    render(renderer);
    renderer.displayBuffer();
    return true;
  }

  void render(const GfxRenderer& renderer) const {
    if (!active) return;
    namespace fui = freeink::ui;

    // Per-render target: a GfxRendererTarget is a renderer reference plus
    // three font ids, so rebuilding it here is trivially cheap and always
    // tracks the live orientation and uiScale fonts; a target held across
    // show() would stale-bind both after a rotation or scale change.
    fui::GfxRendererTarget target = makeUiTarget(renderer);
    target.setFont(fui::GfxRendererTarget::FONT_TITLE,
                   GUI.titleFontId(renderer, headline.empty() ? title.c_str() : headline.c_str()));
    const fui::ThemeTokens& theme = refreshSharedUiThemeTokens(target);
    // Frame stores a const DeviceContext&; keep it in a local that outlives
    // the frame (a deviceContext() temporary would dangle).
    const fui::DeviceContext device = target.deviceContext();
    // Routing happens on the loop task against the member buffer; the frame
    // itself never dispatches, so it gets an empty snapshot.
    const fui::InputSnapshot noInput{};

    // Builds into the generation handleInput()'s routePublished()/
    // publishedData() aren't currently reading, so the loop task never sees
    // this table mid-rebuild — see publish() below and
    // InteractionBuffer::beginPublishCycle().
    interactions.beginPublishCycle();
    fui::Frame<INTERACTION_CAPACITY> frame(target, device, noInput, interactions);

    const auto& metrics = UITheme::getInstance().getMetrics();
    const int totalOptions = static_cast<int>(ownedStrings.size());
    const uint8_t count = static_cast<uint8_t>(totalOptions > MAX_OPTIONS ? MAX_OPTIONS : totalOptions);

    fui::DialogOption options[MAX_OPTIONS];
    for (uint8_t i = 0; i < count; ++i) {
      options[i].label = ownedStrings[i].c_str();
      options[i].action = ACTION_OPTION;
      options[i].value = static_cast<int16_t>(i);
      options[i].state = (i == selectedIndex) ? fui::StateFocused : fui::StateNormal;
    }

    // The render task serializes dialogs; reuse the style-rich props rather
    // than putting both StyleSets on its constrained stack.
    static fui::OptionDialogProps props;
    props.title = title.c_str();
    props.headline = headline.empty() ? nullptr : headline.c_str();
    props.options = options;
    props.optionCount = count;
    props.verticalOptions = true;
    // Touch only: physical buttons stay on the legacy wrap/confirm path above,
    // so the buffer never competes with it for focus/confirm dispatch.
    props.inputMask = fui::InputTouch;
    props.titleText = headline.empty() ? theme.titleText : theme.smallText;
    props.titleText.bold = false;
    props.titleText.align = UITheme::isRtl() ? fui::TextAlign::Right : fui::TextAlign::Left;
    props.titleText.maxLines = 2;
    props.headlineText = theme.titleText;
    props.headlineText.bold = false;
    props.headlineText.align = props.titleText.align;
    props.headlineText.maxLines = 4;
    props.buttonText = theme.bodyText;
    const int16_t innerPadding = static_cast<int16_t>(metrics.optionPopupInnerPadding);
    props.padding = fui::Insets{innerPadding, innerPadding, innerPadding, innerPadding};
    props.gap = static_cast<int16_t>(metrics.optionPopupItemSpacing);
    props.buttonText.maxLines = 2;
    props.buttonStyles = theme.button;
    const bool underlineFocus = theme.listSelectionStyle == fui::SelectionStyle::Underline;
    if (underlineFocus) {
      props.buttonStyles.normal.border = fui::Paint::none();
      props.buttonStyles.normal.borderWidth = 0;
      props.buttonStyles.selected = props.buttonStyles.normal;
      props.buttonStyles.focused = props.buttonStyles.normal;
      props.buttonStyles.active = props.buttonStyles.normal;
    }
    props.styles = theme.popup;
    props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    props.styles.normal.borderWidth = static_cast<uint8_t>(metrics.popupFrameThickness);
    props.styles.normal.radius = static_cast<uint8_t>(metrics.popupCornerRadius);
    props.styles.selected = props.styles.normal;
    props.styles.focused = props.styles.normal;
    props.styles.active = props.styles.normal;
    props.styles.disabled = props.styles.normal;
    props.buttonHeight = fui::clampI16(std::max<int>(
        48, target.lineHeight(theme.bodyText.font) + metrics.optionPopupSelectionVPadding * 2));

    // Bezel-safe area intersected with the front-button hint strip, which
    // follows the physical buttons: bottom in portrait, top inverted, logical
    // left landscape CW, right CCW. Touch boards report no strip.
    const Rect hints = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const fui::Rect bezel = device.safeRect();
    const int16_t usableLeft = static_cast<int16_t>(std::max<int>(bezel.x, hints.x));
    const int16_t usableTop = static_cast<int16_t>(std::max<int>(bezel.y, hints.y));
    const int16_t usableRight = static_cast<int16_t>(std::min<int>(bezel.right(), hints.x + hints.width));
    const int16_t usableBottom = static_cast<int16_t>(std::min<int>(bezel.bottom(), hints.y + hints.height));
    const fui::Rect screen = fui::Rect{usableLeft, usableTop, static_cast<int16_t>(usableRight - usableLeft),
                                       static_cast<int16_t>(usableBottom - usableTop)}
                                 .inset(fui::Insets{theme.spaceLg, 0, theme.spaceLg, 0});
    const int16_t width = fui::clampI16(screen.width - 2 * std::max<int>(
        theme.headerSidePadding, metrics.optionPopupDialogSideMargin));
    const int16_t textWidth = static_cast<int16_t>(width - innerPadding * 2 - 16);
    for (uint8_t i = 0; i < count; ++i) {
      props.buttonHeight = std::max<int16_t>(props.buttonHeight,
          fui::measureWrappedText(target, options[i].label, props.buttonText, textWidth).height + 16);
    }
    props.optionCount = 0;
    int captionHeight = fui::optionDialogHeight(target, props, width);
    // Keep at least one complete action below the captions in landscape.
    while (captionHeight + props.buttonHeight + props.gap > screen.height) {
      if (props.headline && props.headlineText.maxLines > 1) {
        --props.headlineText.maxLines;
      } else if (props.titleText.maxLines > 1) {
        --props.titleText.maxLines;
      } else {
        break;
      }
      captionHeight = fui::optionDialogHeight(target, props, width);
    }
    const int pageSize = std::max(1, std::min<int>(count,
        (screen.height - captionHeight) / (props.buttonHeight + props.gap)));
    visibleOptions = pageSize;
    if (selectedIndex < firstOption) firstOption = selectedIndex;
    if (selectedIndex >= firstOption + pageSize) firstOption = selectedIndex - pageSize + 1;
    firstOption = std::max(0, std::min(firstOption, count - pageSize));
    props.options = options + firstOption;
    props.optionCount = static_cast<uint8_t>(pageSize);
    const int16_t height = fui::clampI16(fui::optionDialogHeight(target, props, width), 0, screen.height);
    const fui::Rect dialogRect = fui::centeredRect(screen, fui::Size{width, height});

    // The framebuffer still contains the previous popup for overlay-only callers.
    // Erase both surfaces before drawing the replacement; a waveform alone cannot
    // remove ink that remains in the source framebuffer.
    constexpr int halo = 8;
    if (painted && (lastRect.x != dialogRect.x || lastRect.y != dialogRect.y ||
                    lastRect.width != dialogRect.width || lastRect.height != dialogRect.height)) {
      renderer.fillRect(lastRect.x - halo, lastRect.y - halo, lastRect.width + halo * 2,
                        lastRect.height + halo * 2, false);
      renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
    }
    renderer.fillRect(dialogRect.x - halo, dialogRect.y - halo, dialogRect.width + halo * 2,
                      dialogRect.height + halo * 2, false);
    lastRect = dialogRect;
    painted = true;

    // Chrome guard first, options after: route() scans newest-first, so the
    // option buttons win inside the dialog and the guard absorbs the rest.
    frame.hit(dialogRect, ACTION_CHROME, 0, fui::InputTouch);
    fui::optionDialog(frame, dialogRect, props);
    if (underlineFocus && selectedIndex >= firstOption && selectedIndex < firstOption + pageSize) {
      const fui::Rect inner = dialogRect.inset(props.padding);
      const int16_t buttonsH = static_cast<int16_t>(pageSize * props.buttonHeight + (pageSize - 1) * props.gap);
      const int16_t selectedBottom = static_cast<int16_t>(inner.bottom() - buttonsH +
          (selectedIndex - firstOption) * (props.buttonHeight + props.gap) + props.buttonHeight);
      target.fill(fui::Rect{inner.x, static_cast<int16_t>(selectedBottom - 2), inner.width, 2},
                  fui::Paint::solid(fui::Color::Black));
    }
    if (pageSize < count) {
      fui::drawListScrollIndicator(target,
          fui::Rect{static_cast<int16_t>(dialogRect.x + 4), static_cast<int16_t>(dialogRect.y + innerPadding),
                    static_cast<int16_t>(dialogRect.width - 8), static_cast<int16_t>(dialogRect.height - innerPadding * 2)},
          count, pageSize, firstOption, theme.listScrollWidth, UITheme::isRtl() ? 1 : 0);
    }
    // Atomically make this generation the one handleInput() reads, now that
    // every hit() call for this frame is done.
    interactions.publish();
    uiReady = true;
  }

  bool isActive() const { return active; }

  // Close without firing the callback (the surface under the popup is going
  // away, e.g. its host screen closes from outside the popup's own input).
  void dismiss() {
    active = false;
    painted = false;
    onSelectCallback = nullptr;
  }

 private:
  // Bounded option window; swipes and button focus reveal the remaining rows.
  // One extra interaction absorbs taps on the modal chrome.
  static constexpr int MAX_OPTIONS = 16;
  static constexpr size_t INTERACTION_CAPACITY = MAX_OPTIONS + 1;
  static constexpr freeink::ui::ActionId ACTION_OPTION = 1;
  static constexpr freeink::ui::ActionId ACTION_CHROME = 2;

  void activate(int currentIndex, std::function<void(int)> onSelect) {
    const int count = std::min<int>(ownedStrings.size(), MAX_OPTIONS);
    selectedIndex = currentIndex >= 0 && currentIndex < count ? currentIndex : 0;
    firstOption = 0;
    onSelectCallback = std::move(onSelect);
    uiReady = false;
    active = count > 0;
  }

  bool active = false;
  std::string title;
  std::string headline;
  std::vector<std::string> ownedStrings;
  int selectedIndex = 0;
  std::function<void(int)> onSelectCallback;
  // Written by the render task (frame registration), routed by the loop task;
  // uiReady closes the rebuild window exactly like UiListActivity::uiReady.
  mutable freeink::ui::InteractionBuffer<INTERACTION_CAPACITY> interactions;
  mutable std::atomic<bool> uiReady{false};
  mutable freeink::ui::Rect lastRect{};
  mutable bool painted = false;
  mutable int firstOption = 0;
  mutable std::atomic<int> visibleOptions{1};
};
