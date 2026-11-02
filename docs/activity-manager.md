# Activity & ActivityManager Migration Guide

This document explains the refactoring from the original per-activity render task model to the centralized `ActivityManager` introduced in [PR #1016](https://github.com/crosspoint-reader/crosspoint-reader/pull/1016). It covers the architectural differences, what changed for activity authors, and the FreeRTOS task and locking model that underpins the system.

## Overview of Changes

| Aspect | Old Model | New Model |
|--------|-----------|-----------|
| Render task | One per activity (8KB stack each) | Single shared task in `ActivityManager` |
| Render mutex | Per-activity `renderingMutex` | Single global mutex in `ActivityManager` |
| `RenderLock` | Inner class of `Activity` | Standalone class, acquires global mutex |
| Subactivities | `ActivityWithSubactivity` base class | Activity stack managed by `ActivityManager` |
| Navigation | Free functions in `main.cpp` | `activityManager.goHome()`, `goToReader()`, etc. |
| Subactivity results | Callback lambdas stored in parent | `startActivityForResult()` / `setResult()` / `finish()` |
| `requestUpdate()` | Notifies activity's own render task | Delegates to `ActivityManager` (immediate or deferred) |

## Architecture

### Old Model: Per-Activity Render Tasks

Each activity created its own FreeRTOS render task on entry and destroyed it on exit:

```text
┌─────────────────────────────────────────────────────────┐
│ Main Task (Arduino loop)                                │
│  ┌───────────────────────────────────────────────────┐  │
│  │ currentActivity->loop()                           │  │
│  │   ├── handle input                                │  │
│  │   ├── update state (under RenderLock)             │  │
│  │   └── requestUpdate()  ──notify──►  Render Task   │  │
│  │                                      (per-activity)│  │
│  │                                      8KB stack     │  │
│  │                                      owns mutex    │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│ ActivityWithSubactivity:                                │
│  ┌──────────────┐     ┌──────────────┐                  │
│  │ Parent        │────►│ SubActivity   │                 │
│  │ (has render   │     │ (has own      │                 │
│  │  task)        │     │  render task) │                 │
│  └──────────────┘     └──────────────┘                  │
└─────────────────────────────────────────────────────────┘
```

Problems with this approach:

- **8KB per render task**: Each activity allocated an 8KB FreeRTOS stack for its render task, even though only one renders at a time
- **Dangerous deletion patterns**: `exitActivity()` + `enterNewActivity()` in callbacks led to `delete this` situations where the caller was destroyed while its code was still on the stack
- **Subactivity coupling**: Parents stored callbacks to child results, creating tight coupling and lifetime hazards

### New Model: Centralized ActivityManager

A single `ActivityManager` owns the render task and manages an activity stack:

```text
┌──────────────────────────────────────────────────────────┐
│ Main Task (Arduino loop)                                 │
│                                                          │
│  activityManager.loop()                                  │
│    │                                                     │
│    ├── currentActivity->loop()                           │
│    │     ├── handle input                                │
│    │     ├── update state (under RenderLock)              │
│    │     └── requestUpdate()                             │
│    │                                                     │
│    ├── process pending actions (Push / Pop / Replace)    │
│    │                                                     │
│    └── if requestedUpdate: ──notify──► Render Task       │
│                                         (single, shared) │
│                                         8KB stack        │
│                                         global mutex     │
│                                                          │
│  Activity Stack:                                         │
│  ┌──────────┬──────────┬──────────┐    ┌──────────┐     │
│  │ Home     │ Settings │ Wifi     │    │ Keyboard │     │
│  │ (stack)  │ (stack)  │ (stack)  │    │ (current)│     │
│  └──────────┴──────────┴──────────┘    └──────────┘     │
│   stackActivities[]                    currentActivity   │
└──────────────────────────────────────────────────────────┘
```

## Navigation data and repaint boundaries

`InputManager` retains X4 Pro button edges and publishes press/release events
through `HalGPIO`; `MappedInputManager` applies the configured logical mapping.
`ButtonNavigator` consumes those edges: Home steps on press, lists step on
release, and holds page through the list. Swipes request viewport movement
without moving the selected row. `requestUpdate(true)` means immediate
scheduling, **not** a full panel refresh.

| Routine move | State and storage work | Layout and requested refresh |
|---|---|---|
| Home button/swipe | Changes `selectorIndex`; loaded recents and cover caches are reused. | Repaints Home with `FAST_REFRESH`; its initial cleanup paint may request `HALF_REFRESH`. |
| Home cover/menu tap | Classic Home selects and paints touch-down feedback, then opens once on release. A committed Library transition may prepare its private index while that feedback waveform runs. | Home feedback and destination retain their existing refresh requests. Cover-grid Home still dispatches on release. |
| Library row / viewport page | Ring/`ListNav` change; the materialized row window is reused while its start, capacity, count and grouping match. Only a window miss reads index records and label fields. | `UiListActivity` builds the list and chrome, then requests the default `FAST_REFRESH`. |
| Library sort, filter, recents or changed index | Invalidates row text before the next layout. An unchanged background scan does not invalidate the window; status-only repaints reuse it. | Same list renderer and refresh request; background publication retains its existing render lock and navigation clamp. |
| File Browser row / page | Navigation operates on resident filenames; only visible names are formatted, and glyphs are prewarmed by window. No directory enumeration per row move. | Shared list renderer, default `FAST_REFRESH`. |
| Folder enter / Back to parent | Changes path, enumerates and sorts the destination directory. Parent return selects the directory just exited and follows that selection. | Shared list renderer, default `FAST_REFRESH`; no new directory cache. |
| Reader menu move | Moves `ListNav`; menu rows are constructed once, with only live values updated on render. | List and chrome repaint, default `FAST_REFRESH`. |
| EPUB / TXT Contents move | Reuses the existing 24-row title window while it covers the required viewport; a miss reads LUT entries / TXT headings and prewarms their glyphs. Current-chapter markers and absolute action indexes stay attached to the cached rows. | Shared list renderer, default `FAST_REFRESH`; the supplied item count bounds the window. |
| Settings move | Moves the tab ring or `ListNav`; structural rows are resident and live setting values are refreshed in place. Directory/font discovery runs on settings-list rebuilds, not row moves. | Shared list renderer, default `FAST_REFRESH`. |
| Back from a child screen | `finish()` queues a pop and the parent repaints from its retained state; Library has staged Back behavior before leaving. | Destination activity chooses its existing mode; no refresh-mode override here. |
| Keyboard header Back | Checks logical Back before text/key touch routing, so a header tap cancels instead of being swallowed as an unmatched key tap. | Parent repaint after pop; no intermediate keyboard refresh. |

These are activity **requests**, not guarantees of a particular electrical
waveform. Renderer promotion, night-mode inversion policy, panel support and
the GC/ghost-cleanup cadence remain authoritative and unchanged. List paints
still reconstruct the framebuffer: data-window reuse avoids SD/decode work,
not the layout or panel waveform.

For classic touch Home→Library replacement only, `ActivityManager` first revokes
background sync/storage ownership. Home's feedback epoch identifies the requested
highlight, and its atomic render phase waits for all drawing/recents writes to
finish before freezing further outgoing work. Library preparation can then overlap
the **existing blocking** Home waveform; it neither redraws the framebuffer nor
changes polarity, refresh mode, completed-frame/OTA acceptance, or render locking.
Home's post-waveform cover loading is skipped only once that outgoing transition
has committed. Physical/cover-grid paths with no classic touch feedback do not wait
for a nonexistent frame and use ordinary entry.

Preparation reads a bounded recents missing mask without modifying the shared
store. If no pruning is needed, it opens/validates the destination's private index
and resolves pins once. Otherwise pruning and its persistence remain under the
entry render lock, before opening the long-lived index reader. `onEnter` consumes
the prepared state once; a changed recents revision discards prepared index/pins
and rechecks the current store rather than applying stale mask positions.

When benchmark recording is enabled, `nav_rows` spans report Library or
Contents window requests: screen ID, cache hit (1) or load (0), and rows
materialized (zero on a hit). Load duration includes SD-backed row reads and
metadata decoding/materialization, not raw SD bus time. Contents glyph prewarm
is outside that span and remains part of total render time. Disabled recording
does not read the profiling clock.
Screen ID 3 (`LibraryPrepare`) instead reports the entry-preparation interval,
with fields `[3, 0, indexed-book-count]` (zero if deferred/missing). Compare that
interval against the outgoing panel BUSY interval to establish actual overlap;
it remains included in input-to-settled destination latency. Host barriers verify
ownership behavior, not a millisecond performance improvement.

Focused host behavior checks (each invokes one host compiler):

```bash
python3 test/test_library_row_window.py
python3 test/test_contents_row_window.py
python3 test/test_home_touch_navigation.py
python3 test/test_list_viewport.py
python3 test/test_keyboard_back_navigation.py
python3 test/test_library_entry_preparation.py
python3 test/test_home_library_preparation.py
python3 test/test_file_browser_delete_recents.py
```

## Migration Checklist

### 1. Change Base Class

If your activity extended `ActivityWithSubactivity`, change it to extend `Activity`:

```cpp
// BEFORE
class MyActivity final : public ActivityWithSubactivity {
  MyActivity(GfxRenderer& r, MappedInputManager& m, std::function<void()> goBack)
      : ActivityWithSubactivity("MyActivity", r, m), goBack(goBack) {}
};

// AFTER
class MyActivity final : public Activity {
  MyActivity(GfxRenderer& r, MappedInputManager& m)
      : Activity("MyActivity", r, m) {}
};
```

Note that navigation callbacks like `goBack` are no longer stored — use `finish()` or `activityManager.goHome()` instead.

### 2. Replace Navigation Functions

The free functions `exitActivity()` / `enterNewActivity()` in `main.cpp` are gone. Use `ActivityManager` methods:

```cpp
// BEFORE (in main.cpp or via stored callbacks)
exitActivity();
enterNewActivity(new SettingsActivity(renderer, mappedInput, onGoHome));

// AFTER (from any Activity method)
activityManager.goToSettings();
// or for arbitrary navigation:
activityManager.replaceActivity(std::make_unique<MyActivity>(renderer, mappedInput));
```

`replaceActivity()` destroys the current activity and clears the stack. Use it for top-level navigation (home, reader, settings, etc.).

### 3. Replace Subactivity Pattern

The `enterNewActivity()` / `exitActivity()` subactivity pattern is replaced by a stack with typed results:

```cpp
// BEFORE
void MyActivity::launchWifi() {
  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
      [this](bool connected) { onWifiDone(connected); }));
}
// Child calls: onComplete(true); // triggers callback, which may call exitActivity()

// AFTER
void MyActivity::launchWifi() {
  startActivityForResult(
      std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        auto& wifi = std::get<WifiResult>(result.data);
        onWifiDone(wifi.connected);
      });
}
// Child calls:
//   setResult(WifiResult{.connected = true, .ssid = ssid});
//   finish();
```

Key differences:

- **`startActivityForResult()`** pushes the current activity onto the stack and launches the child
- **`setResult()`** stores a typed result on the child activity
- **`finish()`** signals the manager to pop the child, call the result handler, and resume the parent
- The parent is never deleted during this process — it's safely stored on the stack

### 4. Update `render()` Signature

The `RenderLock` type changed from `Activity::RenderLock` (inner class) to standalone `RenderLock`:

```cpp
// BEFORE
void render(Activity::RenderLock&&) override;

// AFTER
void render(RenderLock&&) override;
```

Include `RenderLock.h` if not transitively included via `Activity.h`.

### 5. Update `onEnter()` / `onExit()`

Activities no longer create or destroy render tasks:

```cpp
// BEFORE
void MyActivity::onEnter() {
  Activity::onEnter();       // created render task + logged
  // ... allocate resources
  requestUpdate();
}
void MyActivity::onExit() {
  // ... free resources
  Activity::onExit();        // acquired RenderLock, deleted render task
}

// AFTER
void MyActivity::onEnter() {
  Activity::onEnter();       // just logs
  // ... allocate resources
  requestUpdate();
}
void MyActivity::onExit() {
  // ... free resources
  Activity::onExit();        // just logs
}
```

The render task lifecycle is handled entirely by `ActivityManager::begin()`.

### 6. Update `requestUpdate()` Calls

The signature changed to accept an `immediate` flag:

```cpp
// BEFORE
void requestUpdate();  // always immediate notification to per-activity render task

// AFTER
void requestUpdate(bool immediate = false);
// immediate=false (default): deferred until end of current loop iteration
// immediate=true: sends notification to render task right away
```

**When to use `immediate`**: Almost never. Deferred updates are batched — if `loop()` triggers multiple state changes that each call `requestUpdate()`, only one render happens. Use `immediate` only when you need the render to start before the current function returns (e.g., before a blocking network call).

**`requestUpdateAndWait()`**: Blocks the calling task until the render completes. Use sparingly — it's designed for cases where you need the screen to reflect new state before proceeding (e.g., showing "Checking for update..." before calling a network API).

### 7. Remove Stored Navigation Callbacks

Old activities often stored `std::function` callbacks for navigation:

```cpp
// BEFORE
class SettingsActivity : public ActivityWithSubactivity {
  const std::function<void()> goBack;   // stored callback
  const std::function<void()> goHome;   // stored callback
public:
  SettingsActivity(GfxRenderer& r, MappedInputManager& m,
                   std::function<void()> goBack, std::function<void()> goHome)
      : ActivityWithSubactivity("Settings", r, m), goBack(goBack), goHome(goHome) {}
};

// AFTER
class SettingsActivity : public Activity {
public:
  SettingsActivity(GfxRenderer& r, MappedInputManager& m)
      : Activity("Settings", r, m) {}
  // Use finish() to go back, activityManager.goHome() to go home
};
```

This removes `std::function` overhead (~2-4KB per unique signature) and eliminates lifetime risks from captured `this` pointers.

## Technical Details

### FreeRTOS Task Model

The firmware runs on an ESP32-C3, a single-core RISC-V microcontroller. FreeRTOS provides cooperative and preemptive multitasking on this single core — only one task executes at any moment, and the scheduler switches between tasks at yield points (blocking calls, `vTaskDelay`, `taskYIELD`) or when a tick interrupt promotes a higher-priority task.

There are two tasks relevant to the activity system:

```text
┌──────────────────────┐     ┌──────────────────────────┐
│ Main Task            │     │ Render Task              │
│ (Arduino loop)       │     │ (ActivityManager-owned)   │
│ Priority: 1          │     │ Priority: 1              │
│                      │     │                          │
│ Runs:                │     │ Runs:                    │
│ - gpio.update()      │     │ - ulTaskNotifyTake()     │
│ - activity->loop()   │     │   (blocks until notified)│
│ - pending actions    │     │ - RenderLock (mutex)     │
│ - sleep/power mgmt   │     │ - activity->render()     │
│ - requestUpdate →────┼─────┼─► xTaskNotify()          │
│   (end of loop)      │     │                          │
└──────────────────────┘     └──────────────────────────┘
```

Both tasks run at priority 1. Since the ESP32-C3 is single-core, they alternate execution: the main task runs `loop()`, then at the end of the loop iteration, notifies the render task if an update was requested. The render task wakes, acquires the mutex, calls `render()`, releases the mutex, and blocks again.

Do not use `xTaskCreate` inside activities. If you have a use case that seems to require a background task, open a discussion to propose a lifecycle-aware `Worker` abstraction first.

### The Render Mutex and RenderLock

A single FreeRTOS mutex (`renderingMutex`) protects shared state between `loop()` and `render()`. Since these run on different tasks, any state read by `render()` and written by `loop()` must be guarded.

`RenderLock` is an RAII wrapper:

```cpp
// Standalone class (not tied to any specific activity)
class RenderLock {
  bool isLocked = false;
public:
  explicit RenderLock();           // acquires activityManager.renderingMutex
  explicit RenderLock(Activity&);  // same — Activity& param kept for compatibility
  ~RenderLock();                   // releases mutex if still held
  void unlock();                   // early release
};
```

**Usage patterns:**

```cpp
// In loop(): protect state mutations that render() reads
void MyActivity::loop() {
  if (somethingChanged) {
    RenderLock lock;
    state = newState;        // safe — render() can't run while lock is held
  }
  requestUpdate();           // trigger render after lock is released
}

// In render(): lock is passed in, held for duration of render
void MyActivity::render(RenderLock&&) {
  // Lock is held — safe to read shared state
  renderer.clearScreen();
  renderer.drawText(..., stateString, ...);
  renderer.displayBuffer();
  // Lock released when RenderLock destructor runs
}
```

**Critical rule**: Never call `requestUpdateAndWait()` while holding a `RenderLock`. The render task needs the mutex to call `render()`, so holding it while waiting for the render to complete is a deadlock:

```text
Main Task                    Render Task
──────────                   ───────────
RenderLock lock;             (blocked on mutex)
requestUpdateAndWait();
  → notify render task
  → block waiting for
    render to complete        → wakes up
                              → tries to acquire mutex
                              → DEADLOCK: main holds mutex,
                                waits for render; render
                                waits for mutex
```

### requestUpdate() vs requestUpdateAndWait()

```text
requestUpdate(false)          requestUpdate(true)
─────────────────             ─────────────────
Sets flag only.               Notifies render task
Render happens after          immediately.
loop() returns and            Render may start
ActivityManager checks        before the calling
the flag.                     function returns.
                              (Does NOT wait for
                              render to complete.)

requestUpdateAndWait()
──────────────────────
Notifies render task AND
blocks calling task until
render is done. Uses
FreeRTOS direct-to-task
notification on the
caller's task handle.
```

`requestUpdateAndWait()` flow in detail:

```text
Calling Task                 Render Task
────────────                 ───────────
requestUpdateAndWait()
  ├─ assert: not render task
  ├─ assert: not holding RenderLock
  ├─ store waitingTaskHandle
  ├─ xTaskNotify(renderTask)  → wakes render task
  └─ ulTaskNotifyTake() ─┐
     (blocked)           │    RenderLock lock;
                         │    activity->render();
                         │    // render complete
                         │    taskENTER_CRITICAL
                         │    waiter = waitingTaskHandle
                         │    waitingTaskHandle = nullptr
                         │    taskEXIT_CRITICAL
                         │    xTaskNotify(waiter) ───┐
                         │                           │
  ┌──────────────────────┘                           │
  │ (woken by notification) ◄────────────────────────┘
  └─ return
```

### Activity Lifecycle Under ActivityManager

```text
activityManager.replaceActivity(make_unique<MyActivity>(...))
  │
  ▼
╔═══════════════════════════════════════════════════╗
║  pendingAction = Replace                          ║
║  pendingActivity = MyActivity                     ║
╚═══════════════════════════════════════════════════╝
  │
  ▼ (next loop iteration)
  ActivityManager::loop()
  │
  ├── currentActivity->loop()     // old activity's last loop
  │
  ├── process pending action:
  │   ├── RenderLock lock;
  │   ├── oldActivity->onExit()   // cleanup under lock
  │   ├── delete oldActivity
  │   ├── clear stack
  │   ├── currentActivity = MyActivity
  │   ├── lock.unlock()
  │   └── MyActivity->onEnter()   // init new activity
  │
  └── if requestedUpdate:
      └── notify render task
```

For push/pop (subactivity) navigation:

```text
Parent calls: startActivityForResult(make_unique<Child>(...), handler)
  │
  ▼
╔══════════════════════════════════════╗
║  pendingAction = Push               ║
║  pendingActivity = Child            ║
║  parent->resultHandler = handler    ║
╚══════════════════════════════════════╝
  │
  ▼ (next loop iteration)
  ├── Parent moved to stackActivities[]
  ├── currentActivity = Child
  └── Child->onEnter()

        ... child runs ...

Child calls: setResult(MyResult{...}); finish();
  │
  ▼
╔══════════════════════════════════════╗
║  pendingAction = Pop                ║
║  child->result = MyResult{...}      ║
╚══════════════════════════════════════╝
  │
  ▼ (next loop iteration)
  ├── result = child->result
  ├── Child->onExit(); delete Child
  ├── currentActivity = Parent (popped from stack)
  ├── Parent->resultHandler(result)
  └── requestUpdate()   // automatic re-render for parent
```

### OTA startup acceptance

`src/network/OtaBootSwitch.cpp` overrides Arduino's weak C-linkage
`verifyRollbackLater()` hook so the framework cannot accept an OTA image before
`setup()`. After an activity's blocking `render()` returns, the render task calls
`ota_boot::acceptPendingUpdate()` only when storage is ready and either Home was
rendered or `hasCompletedHealthyRender()` reports successful reader content.

The base activity readiness method defaults to false. Reader activities expose
their existing completed-page latch; an end-of-book allocation failure does not
set it. Do not downcast based on `isReaderActivity()`: some reader-related dialogs
return true without inheriting `ReaderActivity`. Splash, recovery, crash, and
SD-error screens cannot certify a healthy startup.

Acceptance is attempted once per boot, only for `ESP_OTA_IMG_PENDING_VERIFY`.
Errors are logged without aborting or retrying, leaving the image unaccepted.
The latch lives in ordinary RAM, not RTC memory. The call changes OTA selection
metadata, not either application image.

With a rollback-enabled bootloader, a reset before acceptance allows fallback to
the remaining bootable slot. Once accepted, later runtime failures do not trigger
this startup rollback; use the explicit alternate-slot recovery controls. A
completed display call is not a physical panel-health test, particularly where
the display driver permits a BUSY timeout.

The final image was exercised with a stock 7.4.4 bootloader in QEMU: both app
addresses booted, early recovery switched slots, and acceptance followed Home's
completed refresh. A pre-Home interruption left the new image pending. QEMU
stalled while programming cold-start bootloader OTA-state changes, so the full
cold-start rollback write remains unverified; emulator checks are not physical
reader verification.

### Common Pitfalls

**Calling `finish()` and continuing to access `this`**: `finish()` sets `pendingAction = Pop` but does not immediately destroy the activity. The activity is destroyed on the next `ActivityManager::loop()` iteration. It's safe to access member variables after `finish()` within the same function, but don't rely on the activity surviving past the current `loop()` call.

**Modifying shared state without `RenderLock`**: If `render()` reads a variable and `loop()` writes it, the write must be under a `RenderLock`. Without it, `render()` could see a half-written value (e.g., a partially updated string or struct).

**Creating background tasks that outlive the activity**: Any FreeRTOS task created in `onEnter()` must be deleted in `onExit()` before the activity is destroyed. The `ActivityManager` does not track or clean up background tasks.

**Holding `RenderLock` across blocking calls**: The render task is blocked on the mutex while you hold the lock. Keep critical sections short — acquire, mutate state, release, then do blocking work.

```cpp
// WRONG — blocks render for the entire network call
void MyActivity::doNetworkStuff() {
  RenderLock lock;
  state = LOADING;
  auto result = http.get(url);  // blocks for seconds with lock held
  state = DONE;
}

// CORRECT — release lock before blocking
void MyActivity::doNetworkStuff() {
  {
    RenderLock lock;
    state = LOADING;
  }
  requestUpdate(true);           // render "Loading..." immediately, before we block
  auto result = http.get(url);   // lock is not held
  {
    RenderLock lock;
    state = DONE;
  }
  requestUpdate();
}
```
