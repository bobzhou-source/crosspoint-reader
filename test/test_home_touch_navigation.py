"""Compile Home's production touch branches and physical-button callbacks.

Run: python3 test/test_home_touch_navigation.py (requires a host C++ compiler).
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/home/HomeActivity.cpp").read_text()
loop = source[source.index("void HomeActivity::loop() {"):source.index("\nvoid HomeActivity::render(")]
start = loop.index("  const HomeLayout layout = homeLayout();")
end = loop.index("\n  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm))", start)
touch = loop[start:end]
start = loop.index("  if (!coverGridUi) {")
end = loop.index("\n  const auto swipe", start)
buttons = loop[start:end]
count_start = source.index("int HomeActivity::getMenuItemCount() const {")
count_method = source[count_start:source.index("\n}", count_start) + 2]

harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <vector>
struct Rect { int x = 0, y = 0, width = 400, height = 200; };
struct HomeLayout { Rect cover, menu; int rowHeight = 44, visibleRows = 4, menuBase = 3, firstRow = 1; };
struct Metrics { int contentSidePadding = 12, homeRecentBooksCount = 3, menuSpacing = 8; };
struct UITheme { static bool rtl; static bool isRtl() { return rtl; } };
bool UITheme::rtl = false;
struct MappedInputManager {
  enum class RowTouch { None, Down, Tap };
  RowTouch cover = RowTouch::None, menu = RowTouch::None;
  int coverIndex = 0, menuIndex = 0;
  RowTouch colTouch(int& index, int, int, int, int, int, int) { index = coverIndex; return cover; }
  RowTouch rowTouch(int& index, int, int, int, int, int, int) { index = menuIndex; return menu; }
};
struct ButtonNavigator {
  bool next = false, previous = false;
  static int nextIndex(int i, int n) { return (i + 1) % n; }
  static int previousIndex(int i, int n) { return (i + n - 1) % n; }
  template<class F> void onNext(F fn) { if (next) fn(); }
  template<class F> void onPrevious(F fn) { if (previous) fn(); }
};
struct HomeActivity {
  MappedInputManager mappedInput;
  ButtonNavigator buttonNavigator;
  std::vector<int> recentBooks{0, 1, 2};
  bool coverGridUi = false;
  bool hasOpdsServers = false;
  std::atomic<unsigned> feedbackRequested{0};
  int selectorIndex = 0, updates = 0, activations = 0, activatedIndex = -1;
  HomeLayout homeLayout() { return {}; }
  int getMenuItemCount() const;
  void requestUpdate() { ++updates; }
  void touchPass();
  void buttonPass();
};
''' + count_method + r'''
void HomeActivity::touchPass() {
  const Metrics metrics;
  auto activateSelection = [this] { ++activations; activatedIndex = selectorIndex; };
''' + touch + r'''
}
void HomeActivity::buttonPass() {
  const int menuCount = getMenuItemCount();
''' + buttons + r'''
}
int main() {
  using Touch = MappedInputManager::RowTouch;
  for (bool rtl : {false, true}) {
    UITheme::rtl = rtl;
    HomeActivity home;
    home.selectorIndex = 7;
    home.mappedInput.coverIndex = 0;
    home.mappedInput.cover = Touch::Down;
    home.touchPass();
    assert(home.updates == 1 && home.activations == 0 && home.selectorIndex == (rtl ? 2 : 0));
    assert(home.feedbackRequested == 1);
    // Down remains true on every poll after the selection delay, including
    // while the render task is drawing or blocked in the first waveform.
    for (int poll = 0; poll < 12; ++poll) home.touchPass();
    assert(home.updates == 1 && home.activations == 0 && home.feedbackRequested == 1);
    home.mappedInput.cover = Touch::None; // cancelled press keeps the visible highlight, but never opens
    home.touchPass();
    assert(home.updates == 1 && home.activations == 0 && home.selectorIndex == (rtl ? 2 : 0));
    home.mappedInput.cover = Touch::Down; // retouch the already highlighted cover
    for (int poll = 0; poll < 3; ++poll) home.touchPass();
    assert(home.updates == 1 && home.activations == 0 && home.feedbackRequested == 1);
    home.mappedInput.cover = Touch::Tap;
    home.touchPass();
    assert(home.updates == 1 && home.activations == 1 && home.activatedIndex == (rtl ? 2 : 0));

    home.mappedInput.cover = Touch::None;
    home.mappedInput.menu = Touch::Down;
    home.mappedInput.menuIndex = 2;
    home.touchPass();
    assert(home.updates == 2 && home.activations == 1 && home.selectorIndex == 6);
    assert(home.feedbackRequested == 2);
    for (int poll = 0; poll < 12; ++poll) home.touchPass();
    assert(home.updates == 2 && home.activations == 1 && home.feedbackRequested == 2);
    home.mappedInput.menu = Touch::None; // cancelling a menu contact also never opens
    home.touchPass();
    home.mappedInput.menu = Touch::Down;
    for (int poll = 0; poll < 3; ++poll) home.touchPass();
    assert(home.updates == 2 && home.activations == 1 && home.feedbackRequested == 2);
    home.mappedInput.menu = Touch::Tap;
    home.touchPass();
    assert(home.updates == 2 && home.activations == 2 && home.activatedIndex == 6);

    home.mappedInput.menu = Touch::Down;
    home.mappedInput.menuIndex = 1; // a genuinely different selection gets a new epoch
    for (int poll = 0; poll < 12; ++poll) home.touchPass();
    assert(home.updates == 3 && home.activations == 2 && home.selectorIndex == 5);
    assert(home.feedbackRequested == 3);
    home.mappedInput.menu = Touch::Tap;
    home.touchPass();
    assert(home.updates == 3 && home.activations == 3 && home.activatedIndex == 5);

    // A quick tap can release before delayed Down; it must still choose/open
    // its target without first queuing a redundant outgoing Home frame.
    home.mappedInput.menu = Touch::None;
    home.mappedInput.cover = Touch::Tap;
    home.touchPass();
    assert(home.updates == 3 && home.activations == 4 && home.activatedIndex == (rtl ? 2 : 0));
    home.mappedInput.cover = Touch::None;
    home.mappedInput.menu = Touch::Tap;
    home.mappedInput.menuIndex = 0;
    home.touchPass();
    assert(home.updates == 3 && home.activations == 5 && home.activatedIndex == 4);
    assert(home.feedbackRequested == 3);
  }
  UITheme::rtl = false;
  for (bool cover : {false, true}) {
    HomeActivity selected;
    selected.selectorIndex = cover ? 0 : 6;
    selected.mappedInput.cover = cover ? Touch::Down : Touch::None;
    selected.mappedInput.menu = cover ? Touch::None : Touch::Down;
    selected.mappedInput.menuIndex = 2;
    for (int poll = 0; poll < 12; ++poll) selected.touchPass();
    assert(selected.updates == 0 && selected.activations == 0 && selected.feedbackRequested == 0);
    selected.mappedInput.cover = cover ? Touch::Tap : Touch::None;
    selected.mappedInput.menu = cover ? Touch::None : Touch::Tap;
    selected.touchPass();
    assert(selected.updates == 0 && selected.activations == 1 && selected.activatedIndex == (cover ? 0 : 6));
    assert(selected.feedbackRequested == 0); // no phantom feedback frame for preparation to wait on
  }
  HomeActivity buttons;
  buttons.buttonNavigator.next = true;
  buttons.buttonPass();
  assert(buttons.selectorIndex == 1 && buttons.updates == 1);
  buttons.buttonNavigator.next = false;
  buttons.buttonNavigator.previous = true;
  buttons.buttonPass();
  assert(buttons.selectorIndex == 0 && buttons.updates == 2);
  assert(buttons.feedbackRequested == 0);
  buttons.recentBooks.clear();
  buttons.buttonPass();
  assert(buttons.selectorIndex == 5 && buttons.updates == 3);  // list home: five fixed rows plus Surprise me
  buttons.hasOpdsServers = true;
  buttons.selectorIndex = 0;
  buttons.buttonPass();
  assert(buttons.selectorIndex == 6 && buttons.updates == 4);  // the optional OPDS row adds one more
  buttons.buttonNavigator.previous = false;
  buttons.buttonNavigator.next = true;
  buttons.buttonPass();
  assert(buttons.selectorIndex == 0 && buttons.updates == 5);
  puts("Home touch: held-Down coalescing, selected-row and quick taps, cancellation, RTL, epoch and physical-button guards");
}
'''

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "home_touch_navigation.cpp"
    executable = Path(directory) / "home_touch_navigation"
    cpp.write_text(harness)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
