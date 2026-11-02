#pragma once

namespace benchmark::nav {

// Built-in script: from the Benchmark screen to a fresh Home (one `do=goto:home` Replace, never Back presses whose count
// depends on the Settings focus and tab the run was started from), then Home, Library, Files and Settings moves.
// Every step is non-mutating: no book is opened, no setting toggled, nothing deleted. Book, reader, dictionary,
// statistics and sleep/wake coverage needs fixture-dependent steps and comes from the optional SD script.
// Home reopens with the row of the screen it came from selected (ActivityManager::goHome; goto:home selects Settings,
// as a Back out of Settings does), and its rows are the
// recent books (0..3, theme dependent) followed by Library, Reading Inbox, Files, [OPDS], File Transfer, Settings.
// Every jump below is therefore counted from that known row toward the end of the list, which does not depend on
// how many recents there are; only a configured OPDS server adds a row (`needs=opds|noopds`).
// Step ids are the stable semantic ids shared with the emulator runner.
inline constexpr char DEFAULT_SCRIPT[] =
    "# built-in navigation script v1\n"
    "prep.home guard=BenchmarkSettings do=goto:home expect=Home setup\n"
    "home.move.0 guard=Home key=Down\n"
    "home.move.1 guard=Home key=Down\n"
    "home.move.2 guard=Home key=Up\n"
    "home.move.3 guard=Home key=Up\n"
    "home.library.open guard=Home needs=noopds lead=Up*4 key=Confirm expect=Library\n"
    "home.library.open.opds guard=Home needs=opds lead=Up*5 key=Confirm expect=Library\n"
    "library.move.0 guard=Library key=Down\n"
    "library.move.1 guard=Library key=Up\n"
    "home.library.back guard=Library key=Back expect=Home\n"
    "home.files.open guard=Home lead=Down*2 key=Confirm expect=FileBrowser\n"
    "files.move.0 guard=FileBrowser key=Down\n"
    "files.move.1 guard=FileBrowser key=Up\n"
    "home.files.back guard=FileBrowser key=Back expect=Home\n"
    "home.settings.open guard=Home needs=noopds lead=Down*2 key=Confirm expect=Settings\n"
    "home.settings.open.opds guard=Home needs=opds lead=Down*3 key=Confirm expect=Settings\n"
    "settings.move.0 guard=Settings key=Down\n"
    "settings.move.1 guard=Settings key=Down\n"
    "settings.move.2 guard=Settings key=Up\n"
    "settings.move.3 guard=Settings key=Up\n"
    "home.settings.back guard=Settings key=Back expect=Home\n"
    "sleep.home guard=Home external\n"
    "wake.home guard=Home external\n";

}  // namespace benchmark::nav
