"""Exercise the production keyboard loop's Back/text/key dispatch ordering.

Run: python3 test/test_keyboard_back_navigation.py (requires a host C++ compiler).
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/util/KeyboardEntryActivity.cpp").read_text()
start = source.index("void KeyboardEntryActivity::loop() {")
end = source.index("\n  if (!cursorMode && mappedInput.wasPressed(MappedInputManager::Button::Up))", start)
# Compile the real input-dispatch prefix. The remaining physical key editing
# branches are unchanged; reaching them is observable through fallthrough.
loop = source[start:end] + "\n  ++fallthrough;\n}\n"

harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <components/HeaderBackTapTarget.h>
unsigned long millis() { return 100; }
namespace fui {
struct Event { bool valid = false; int value = 0; bool longPress = false; explicit operator bool() const { return valid; } };
struct TouchHoldRouter {
  struct Result { Event event; bool activeChanged = false; };
  int calls = 0, resets = 0;
  Result result;
  void reset() { ++resets; }
  Result update(int, bool, int16_t, int16_t, bool, int16_t, int16_t, bool, unsigned long) { ++calls; return result; }
};
}
struct MappedInputManager {
  enum class Button { Back };
  bool tapped = false, down = false, physicalBack = false;
  int x = 30, y = 73;
  bool wasReleased(Button) const { return physicalBack || (tapped && HeaderBackTapTarget::contains(x, y)); }
  bool wasScreenTapped(int& tx, int& ty) const { tx = x; ty = y; return tapped; }
  bool wasScreenTouchDown(int& tx, int& ty) const { tx = x; ty = y; return down; }
  bool isScreenTouchHeld(int&, int&) const { return down; }
};
struct KeyboardEntryActivity {
  MappedInputManager mappedInput;
  fui::TouchHoldRouter touchRouter;
  std::string text = "search";
  size_t cursorPos = 0;
  bool cursorMode = false, togglePos = false, hintVisible = false, interactionsReady = true, fieldHit = false;
  int interactions = 0, cancelled = 0, updates = 0, activated = 0, selected = -1, fallthrough = 0;
  bool cursorPositionFromPoint(int, int, size_t& pos) { pos = 2; return fieldHit; }
  void onCancel() { ++cancelled; }
  void requestUpdate() { ++updates; }
  void syncSelectionToValue(int value) { selected = value; }
  bool activateValue(int, bool) { ++activated; return true; }
  void loop();
};
''' + loop + r'''
int main() {
  HeaderBackTapTarget::set(0, 48, 48, 48);
  for (bool cursorMode : {false, true}) {
    KeyboardEntryActivity keyboard;
    keyboard.cursorMode = cursorMode;
    keyboard.mappedInput.tapped = true;
    keyboard.loop();
    assert(keyboard.cancelled == 1 && keyboard.touchRouter.calls == 0);
    assert(keyboard.activated == 0 && keyboard.updates == 0 && keyboard.fallthrough == 0);
  }
  KeyboardEntryActivity physical;
  physical.mappedInput.physicalBack = true;
  physical.loop();
  assert(physical.cancelled == 1 && physical.touchRouter.calls == 0);

  KeyboardEntryActivity key;
  key.mappedInput.tapped = true;
  key.mappedInput.x = 200; key.mappedInput.y = 500;
  key.touchRouter.result.event = {true, 42, false};
  key.loop();
  assert(key.cancelled == 0 && key.activated == 1 && key.selected == 42 && key.updates == 1);
  KeyboardEntryActivity outside;
  outside.mappedInput.tapped = true;
  outside.mappedInput.x = 200; outside.mappedInput.y = 300;
  outside.loop();
  assert(outside.cancelled == 0 && outside.activated == 0 && outside.fallthrough == 0);
  KeyboardEntryActivity field;
  field.mappedInput.tapped = true;
  field.mappedInput.x = 200; field.mappedInput.y = 140;
  field.fieldHit = true;
  field.loop();
  assert(field.cancelled == 0 && field.cursorPos == 2 && field.updates == 1 && field.touchRouter.calls == 0);
  HeaderBackTapTarget::clear();
  KeyboardEntryActivity noHeader;
  noHeader.mappedInput.tapped = true;
  noHeader.loop();
  assert(noHeader.cancelled == 0 && noHeader.touchRouter.calls == 1);
  puts("Keyboard Back dispatch precedes unmatched touch consumption; key/field routing preserved");
}
'''

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "keyboard_back_navigation.cpp"
    executable = Path(directory) / "keyboard_back_navigation"
    cpp.write_text(harness)
    subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src"),
        str(cpp), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
