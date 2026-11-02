"""Exercise the production Home feedback/Library-preparation handoff.

The panel double is a controlled BUSY barrier, not a performance estimate.
Run: python3 test/test_home_library_preparation.py.
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
home_source = (ROOT / "src/activities/home/HomeActivity.cpp").read_text()
home_header = (ROOT / "src/activities/home/HomeActivity.h").read_text()
manager_source = (ROOT / "src/activities/ActivityManager.cpp").read_text()


def method(source, signature):
    start = source.index(signature)
    end = source.index("\n}", start) + 2
    return source[start:end]


guard = method(home_source, "bool HomeActivity::prepareLibraryTransition(")
render = method(home_source, "void HomeActivity::render(")
begin = render[render.index("  if (!coverGridUi) {"):render.index("  renderer.clearScreen();")]
prepared = render[render.index("  // All Home SD/recents mutations"):render.index("  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);")]
display_line = "  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);"
after_display = render[render.index(display_line) + len(display_line):]
post_guard = after_display[:after_display.index("\n  if (!firstRenderDone)")]
idle_line = next(line for line in after_display.splitlines() if "navigationFramePhase.store(NavigationFramePhase::Idle" in line)
fields_start = home_header.index("  enum class NavigationFramePhase")
fields_end = home_header.index("\n", home_header.index("feedbackPrepared{0}", fields_start))
fields = home_header[fields_start:fields_end]
hook_start = manager_source.index("    reader_sync::cancelAndJoin();")
hook_end = manager_source.index("    if (pendingAction == PendingAction::Pop)", hook_start)
hook = manager_source[hook_start:hook_end]

harness = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
std::atomic<bool> delaySeen{false};
void vTaskDelay(int) { delaySeen.store(true); std::this_thread::yield(); }
void waitFor(const std::atomic<bool>& flag) { while (!flag.load()) std::this_thread::yield(); }
struct HalDisplay { enum Mode { FAST_REFRESH, HALF_REFRESH }; };
struct Activity {
  std::string name;
  explicit Activity(std::string n) : name(std::move(n)) {}
  virtual ~Activity() = default;
  virtual bool isHomeActivity() const { return false; }
  virtual void prepareEnter() {}
};
struct HomeActivity : Activity {
  HomeActivity() : Activity("Home") {}
  bool coverGridUi = false;
  bool firstRenderDone = true, cleanInitialRefresh = false;
''' + fields + r'''
  int draws = 0, postWork = 0;
  std::function<void()> duringDrawing = [] {};
  struct Renderer {
    std::function<void()> busy = [] {};
    int displays = 0;
    HalDisplay::Mode lastMode = HalDisplay::FAST_REFRESH;
    void displayBuffer(HalDisplay::Mode mode) { ++displays; lastMode = mode; busy(); }
  } renderer;
  bool isHomeActivity() const override { return true; }
  bool prepareLibraryTransition();
  void render();
};
''' + guard + r'''
void HomeActivity::render() {
''' + begin + r'''
  ++draws;
  duringDrawing(); // includes Home's failed-cover RECENT_BOOKS.updateBook/save
''' + prepared + display_line + post_guard + r'''
  ++postWork; // represents the existing post-waveform loadRecentCovers phase
''' + idle_line + r'''
}
std::vector<std::string> order;
bool radioStopped = false, transitionLocked = false;
namespace reader_sync {
void cancelAndJoin() { radioStopped = true; order.push_back("cancel"); }
}
struct Destination : Activity {
  int prepares = 0;
  explicit Destination(const char* name) : Activity(name) {}
  void prepareEnter() override {
    assert(radioStopped && !transitionLocked);
    ++prepares;
    order.push_back("prepare");
  }
};
struct Manager {
  enum class PendingAction { None, Replace, Push, Pop };
  PendingAction pendingAction = PendingAction::Replace;
  std::unique_ptr<Activity> currentActivity;
  std::unique_ptr<Destination> pendingActivity;
  void transition() {
''' + hook + r'''
    transitionLocked = true;
    order.push_back("lock");
  }
};
int main() {
  {
    HomeActivity physical;
    assert(!physical.prepareLibraryTransition() && !delaySeen.load());
    physical.coverGridUi = true;
    physical.feedbackRequested = 1;
    assert(!physical.prepareLibraryTransition() && !delaySeen.load());
  }
  {
    HomeActivity home;
    home.feedbackRequested = 1;
    std::atomic<bool> drawing{false}, allowDrawEnd{false}, busy{false}, allowWaveEnd{false}, preparedData{false};
    std::atomic<int> recentsRevision{0};
    home.duringDrawing = [&] {
      drawing.store(true);
      waitFor(allowDrawEnd);
      recentsRevision.store(1); // a cover failure persists recents before preparation
    };
    home.renderer.busy = [&] { busy.store(true); waitFor(allowWaveEnd); busy.store(false); };
    std::thread renderTask([&] { home.render(); });
    waitFor(drawing);
    delaySeen = false;
    std::thread loopTask([&] {
      assert(home.prepareLibraryTransition());
      assert(recentsRevision.load() == 1);
      preparedData.store(true);
    });
    waitFor(delaySeen);
    assert(!preparedData.load()); // no SD preparation while Home mutates recents
    allowDrawEnd = true;
    waitFor(busy);
    waitFor(preparedData);
    assert(home.navigationFramePhase == HomeActivity::NavigationFramePhase::Frozen);
    assert(busy.load()); // real state overlap in the double, not a timing claim
    allowWaveEnd = true;
    renderTask.join();
    loopTask.join();
    assert(home.postWork == 0 && home.renderer.displays == 1);
    assert(home.renderer.lastMode == HalDisplay::FAST_REFRESH);
    home.render(); // queued outgoing repaint must not restart recents/cover work
    assert(home.draws == 1 && home.renderer.displays == 1);
    assert(home.prepareLibraryTransition()); // committed handoff is idempotent
  }
  {
    HomeActivity home;
    home.feedbackPrepared = 1;
    home.feedbackRequested = 2;
    delaySeen = false;
    std::atomic<bool> preparedData{false};
    std::thread loopTask([&] { assert(home.prepareLibraryTransition()); preparedData = true; });
    waitFor(delaySeen);
    assert(!preparedData.load()); // the previous frame cannot satisfy a newer Down
    home.render();
    loopTask.join();
    assert(home.feedbackPrepared == 2);
  }
  {
    HomeActivity home;
    home.cleanInitialRefresh = true;
    home.firstRenderDone = false;
    home.render();
    assert(home.renderer.lastMode == HalDisplay::HALF_REFRESH);
    assert(home.postWork == 1 && home.navigationFramePhase == HomeActivity::NavigationFramePhase::Idle);
    home.firstRenderDone = true;
    home.render();
    assert(home.renderer.lastMode == HalDisplay::FAST_REFRESH && home.postWork == 2);
  }
  for (int scenario = 0; scenario < 5; ++scenario) {
    Manager manager;
    auto home = std::make_unique<HomeActivity>();
    home->feedbackRequested = scenario == 1 ? 0 : 1;
    home->feedbackPrepared = home->feedbackRequested.load();
    home->navigationFramePhase = HomeActivity::NavigationFramePhase::Waveform;
    manager.currentActivity = std::move(home);
    manager.pendingActivity = std::make_unique<Destination>(scenario == 2 ? "Settings" : "Library");
    if (scenario == 3) manager.currentActivity = std::make_unique<Activity>("Settings");
    if (scenario == 4) manager.pendingAction = Manager::PendingAction::Push;
    order.clear(); radioStopped = false; transitionLocked = false;
    manager.transition();
    assert(manager.pendingActivity->prepares == (scenario == 0 ? 1 : 0));
    const std::vector<std::string> expected = scenario == 0
        ? std::vector<std::string>{"cancel", "prepare", "lock"}
        : std::vector<std::string>{"cancel", "lock"};
    assert(order == expected);
  }
  puts("Home/Library handoff: feedback epoch, recents-writer exclusion, waveform overlap, physical fallback, lifecycle order");
}
'''

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "home_library_preparation.cpp"
    executable = Path(directory) / "home_library_preparation"
    cpp.write_text(harness)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                    str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=30)
