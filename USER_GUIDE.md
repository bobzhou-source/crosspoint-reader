# CrossPoint User Guide

Welcome to the **CrossPoint** firmware. This guide outlines the hardware controls, navigation, and reading features of the device.

**What's new in rc9.3 (release candidate, not installed by this build):** tap an EPUB picture to view it full-screen; hold a word for about half a second to open its dictionary entry; optionally enable `image_clear_region` to clear just the previous pictures' area. Region clearing is off by default and still requires `clear_after_image: true`—the flash-free fast-picture card policy sets that to false, so adding the region key alone does not enable picture clears. Touch priority is word holds, link taps, picture taps, then the normal menu/page-turn zones; configured chapter-skip or rotation holds keep their zones.

- [CrossPoint User Guide](#crosspoint-user-guide)
  - [1. Hardware Overview](#1-hardware-overview)
    - [Button Layout](#button-layout)
    - [Taking a Screenshot](#taking-a-screenshot)
    - [Frontlight (X4 Pro only)](#frontlight-x4-pro-only)
  - [2. Power \& Startup](#2-power--startup)
    - [Power On / Off](#power-on--off)
    - [First Launch](#first-launch)
  - [3. Screens](#3-screens)
    - [3.1 Home Screen](#31-home-screen)
    - [3.2 Reading Mode](#32-reading-mode)
    - [3.3 Browse Files Screen](#33-browse-files-screen)
    - [3.4 Library Screen](#34-library-screen)
    - [3.5 File Transfer Screen](#35-file-transfer-screen)
    - [3.5.1 Calibre Wireless Transfers](#351-calibre-wireless-transfers)
      - [Installing the Plugin in Calibre](#installing-the-plugin-in-calibre)
      - [Configuring the CrossPoint Plugin in Calibre](#configuring-the-crosspoint-plugin-in-calibre)
      - [Uploading Books](#uploading-books)
      - [Removing a Book](#removing-a-book)
    - [3.6 Settings](#36-settings)
      - [3.6.1 Display](#361-display)
      - [3.6.2 Reader](#362-reader)
      - [3.6.3 Controls](#363-controls)
      - [3.6.4 System](#364-system)
      - [3.6.5 OPDS Servers (Multiple Libraries)](#365-opds-servers-multiple-libraries)
      - [3.6.6 Web Settings (Wi-Fi + OPDS)](#366-web-settings-wi-fi--opds)
      - [3.6.7 KOReader Sync Quick Setup](#367-koreader-sync-quick-setup)
        - [Option A: CrossPoint Sync Server (`sync.crosspointreader.com`, default)](#option-a-crosspoint-sync-server-synccrosspointreadercom-default)
        - [Option B: Legacy Public KOReader Server (`sync.koreader.rocks`)](#option-b-legacy-public-koreader-server-synckoreaderrocks)
        - [Option C: Self-Hosted Server (Docker Compose)](#option-c-self-hosted-server-docker-compose)
        - [Syncing While Reading](#syncing-while-reading)
    - [3.7 Sleep Screen](#37-sleep-screen)
      - [Cover settings](#cover-settings)
      - [Custom images](#custom-images)
    - [3.8 Custom Fonts (SD Card)](#38-custom-fonts-sd-card)
  - [4. Reading Mode](#4-reading-mode)
    - [Page Turning](#page-turning)
    - [Chapter Navigation](#chapter-navigation)
    - [Auto Page Turn](#auto-page-turn)
    - [Tilt Page Turn (X3 only)](#tilt-page-turn-x3-only)
    - [Footnote Navigation](#footnote-navigation)
    - [Picture Viewer](#picture-viewer)
    - [Dictionary Lookup](#dictionary-lookup)
    - [System Navigation](#system-navigation)
    - [Supported Languages](#supported-languages)
  - [5. Reader Menu](#5-reader-menu)
    - [5.1 Chapter Selection](#51-chapter-selection)
    - [5.2 Bookmarks](#52-bookmarks)
  - [6. Current Limitations \& Roadmap](#6-current-limitations--roadmap)
  - [7. Troubleshooting Issues \& Escaping Bootloop](#7-troubleshooting-issues--escaping-bootloop)

## 1. Hardware Overview

The device utilises the standard buttons on the Xteink X4 (in the same layout as the manufacturer firmware, by default):

### Button Layout

| Location        | Buttons                                              |
| --------------- | ---------------------------------------------------- |
| **Bottom Edge** | **Back**, **Confirm**, **Left**, **Right**           |
| **Right Side**  | **Power**, **Side Up**, **Side Down**, **Reset** |

Button layout can be customized in the **[Controls Settings](#363-controls)**.

### Taking a Screenshot

When the Power button and the lower side button (Side Down) are pressed at the same time, it will take a screenshot and save it in the folder `screenshots/`.

Alternatively, while reading a book, press the **Confirm** button to open the reader menu and select **Take screenshot**.

### Frontlight (X4 Pro only)

The X4 Pro has a built-in frontlight with adjustable brightness and warmth. It is controlled from a swipe panel rather than the Settings menu:

* **Open the frontlight panel:** Swipe down from the top edge of the screen, from almost any screen (Home, Browse Files, Reading Mode, etc.). Drag the brightness and warmth sliders to adjust the light live, or tap the sun icon to turn it on or off.
* **Quick toggle:** Double-click the **Power** button to turn the frontlight on or off instantly, without opening the panel.

> [!NOTE]
> Frontlight brightness and warmth are intentionally not listed in **[Display Settings](#361-display)** — the swipe panel is the only place to adjust them. The on/off state can also be toggled with the Power-button double-click above.

If the frontlight doesn't come back on after the device wakes from sleep, check **Restore Light on Wake** in **[Display Settings](#361-display)** (on by default). Turning it off is intentional if you'd rather have the light stay off on wake and switch it on yourself each time — but it's easy to forget you changed it.

Brightness is still 0–100. **1% is now four times dimmer** than the old 1% (one step of the 12-bit PWM, instead of one step of 10-bit). A saved brightness of 30% or higher looks the same as before. A saved 1% comes back as 2%, which is the old floor, and the new 1% is the dimmer level below it. The on-device −/+ buttons and the Left/Right keys move 1% per tap. Holding Left or Right repeats, and above 10% the repeat steps by 2. The web settings list also moves 1% per step.

Warmth is front-loaded. **10% warmth is about a third of the warm LED**, not a tenth, so low settings are actually yellow. **100% warmth turns the cool LED fully off.** That is as yellow as this hardware can go. There is no yellower mix past 100%. A yellower look than that only comes from lowering the brightness, which cuts whatever cool light is still on.

An optional file, `/.crosspoint/frontlight.json`, overrides the compiled curves. If the file is missing, unreadable, empty, or not a JSON object, the compiled curves are used. Out-of-range numbers are clamped. Delete the file to go back. Unknown keys are ignored.

| Key | Default | Accepted |
| --- | --- | --- |
| `brightnessGamma` | 1.6554 | 1.0–2.5. The curve from 30% up. Leave it alone unless you want mid and high brightness to change. |
| `lowGamma` | 1.8 | 1.2–3.0. How fast the bottom of the scale climbs up to 30%. |
| `brightnessJoinPercent` | 30 | 10–50. Where the low curve meets the old gamma. |
| `warmthLowPercent` / `warmthLowShare` | 10 / 333 | Interior anchor. Share is thousandths of the warm LED (1–999). |
| `warmthMidPercent` / `warmthMidShare` | 40 / 560 | Must sit strictly between the low and tail anchors. |
| `warmthTailPercent` / `warmthTailShare` | 90 / 780 | 90% still leaves cool light on, so the last part of the slider keeps getting yellower. |
| `warmEfficacy` | 1.0 | 0.5–2.0. 1.0 adds no extra warm duty. There is no measured efficacy for these LEDs; raise it only if warm looks dimmer than cool at the same setting. |

A warmth override that is not strictly increasing is ignored and the compiled warmth curve is kept. 100% warmth is always the warm LED alone, whatever the file says.

---

## 2. Power & Startup

### Power On / Off

To turn the device on or off, **press and hold the Power button for approximately half a second**.
In the **[Controls Settings](#363-controls)** you can configure the power button to turn the device off with a short press instead of a long one.

To reboot the device (for example after a firmware update or if it's frozen), press and release the Reset button, and then quickly press and hold the Power button for a few seconds.

### First Launch

Upon turning the device on for the first time, you will be placed on the **[Home](#31-home-screen)** screen.

> [!NOTE]
> On subsequent restarts, the firmware will automatically reopen the last book you were reading.

---

## 3. Screens

### 3.1 Home Screen

The Home screen is the main entry point to the firmware. From here you can navigate to **[Reading Mode](#4-reading-mode)** with the most recently read book, **[Browse Files](#33-browse-files-screen)**, the **[Library](#34-library-screen)**, **[File Transfer](#35-file-transfer-screen)**, or **[Settings](#36-settings)**.

On classic Home layouts, touching a different cover or menu row highlights it once. Keeping your finger down does not repeatedly refresh the same highlight. Releasing opens the target even when it was already selected; cancelling the touch leaves the highlight without opening it.

### 3.2 Reading Mode

See [Reading Mode](#4-reading-mode) below for more information.

### 3.3 Browse Files Screen

The Browse Files screen acts as a file and folder browser. The full path to the current directory is shown at the top of the screen. File extensions are displayed alongside each filename, and directories are shown with brackets (e.g. `[folder-name]`). Hidden directories (those beginning with `.`) are also visible.

* **Navigate List:** Use **Left** (or **Side Up**), or **Right** (or **Side Down**) to move the selection cursor up and down through folders and books. You can also long-press these buttons to scroll a full page up or down.
* **Open Selection:** Press **Confirm** to open a folder or start reading a selected book. Selecting a `.bmp` file will open the image viewer.
* **Delete Files or Folders:** Hold and release **Confirm** to delete the selected file or folder. You will be given an option to either confirm or cancel. Multiple files can be selected for deletion in a single operation. A deleted book also leaves the recent books on Home and the Library's **Recent** pins; deleting a folder removes every recent book inside it. If a delete fails, the books still on the card stay listed.
* **Rename or Move:** Files can be renamed or moved to a different folder from within the browse screen.

### 3.4 Library Screen

The Library indexes up to 4,096 supported books on the SD card and shows their titles and authors without requiring you to remember their folders. Its three tabs provide different views. An arrow beside the active tab shows the sort direction:

- **Recent** pins the ten books you opened most recently above books sorted by file modification time, newest first. Reverse the direction for oldest first without the pinned overlay. Hold a book for Open, Remove from recents (when pinned), Delete, or Rebuild.
- **Title** groups books by the first letter of the title. Up sorts A-Z and down sorts Z-A. Titles beginning with numbers or punctuation appear under `#`; letters from non-English scripts, including Hebrew, have their own groups.
- **Author** groups books by author. Up sorts A-Z and down sorts Z-A.

On a button-only device:

- Use **Up/Down** or **Left/Right** to move one row at a time. Hold a direction to move a page at a time.
- Press **Confirm** to open the selected book.
- Press **Back** from the book list to focus the tabs. Use **Left/Right** to select another tab, press **Confirm** to reverse its sort direction, or press **Down** to return to the list.
- While the tabs are focused, use **Left** to open Search, or hold **Confirm** to reverse the sort direction.
- In the Title or Author views, hold **Confirm** on a book to collapse the list to its letter or author groups. The matching group remains selected. Press **Confirm** to enter a group, or **Back** to restore the exact book and position you came from.

On a touch device, tap tabs, books, and the Search icon directly. Tap the active tab again to reverse its sort direction. Swipe to scroll. Long-press a book in Recent to open its options. Long-press a book in Title or Author to collapse to the group list, then tap a group to expand it.

The index is created automatically the first time Library opens. Later transfers mark it for refresh. Your existing shelf stays available while new or changed books are indexed in the background; the bottom readout shows progress. You can navigate and open existing books during the refresh. Leaving Library pauses the build after the current metadata operation; completed metadata is saved and reused when you return, including after a restart. The new shelf replaces the old one only when complete. On a first build, Browse Files remains available without waiting for an index.

Use the Library refresh icon or the Recent row's **Rebuild** action to check for file changes. **Use book metadata** controls whether titles and authors are read from EPUB metadata. Unchanged books reuse indexed metadata; metadata indexing does not generate covers.

### 3.5 File Transfer Screen

The File Transfer screen allows you to upload and manage files on the device. When you enter the screen, choose **Join a Network**, **Calibre Wireless**, or **Create Hotspot**. The reader then starts the web server for the selected mode.

See the [web server docs](./docs/webserver.md) for more information on how to connect to the web server and upload files.

The web interface also supports **WebDAV**, allowing you to mount the device as a network drive and manage files directly from your computer's file manager.

Download links for files already on the device are available in the web interface, so you can retrieve books or screenshots over Wi-Fi without connecting a cable.

A **Wi-Fi signal strength indicator** (dBm) is displayed on-screen during joined-network web server sessions.

> [!TIP]
> Advanced users can also manage files programmatically or via the command line using `curl`. See the [web server docs](./docs/webserver.md) for details.
> [!TIP]
> If your EPUBs have compatibility issues, you can run the built-in **EPUB Optimizer** directly from the device to clean up and reprocess books for better rendering.

### 3.5.1 Calibre Wireless Transfers

CrossPoint supports sending books from Calibre using the CrossPoint Reader device plugin.

#### Installing the Plugin in Calibre

If you don't already have the plugin installed:

1. Head to https://github.com/crosspoint-reader/calibre-plugins/releases to download the latest version of the crosspoint_reader plugin.
2. Download the zip file.
3. Open Calibre → Preferences → Plugins → Load plugin from file → Select the zip file.
4. Restart Calibre.

#### Configuring the CrossPoint Plugin in Calibre
1. In Calibre select Preferences.
2. In the Preferences dialog select Plugins.
3. In Plugins search for "crosspoint".
4. Click on "Customize plugin".
5. Update the value for "Host" to match the IP for your device.
6. Leave the other settings as they are.
7. [optional] Modify the "Upload path" to point to a subfolder other than the root "/" folder. Enter this as a path relative to the root folder. Example: `/mybooks`
8. Restart Calibre.

<img width="420" height="385" alt="Image" src="https://github.com/user-attachments/assets/01fc7e33-a9a7-48ba-9e26-2e68d1f9daec" />

#### Uploading Books

To upload a book using the CrossPoint plugin in Calibre:

1. On the device: File Transfer -> Calibre Wireless, then join a network.
2. Select one or more books.
3. Right-click on that selection.
4. Select "Send to Device" > "Send to main memory"

The CrossPoint plugin will connect to your device, create a folder for the book's author in the root folder (or the folder you configured for the plugin), then copy the book into that folder.

<img width="783" height="310" alt="Image" src="https://github.com/user-attachments/assets/741b0909-2e1d-4f16-8af0-2c43fbda5ce6" />

#### Removing a Book

Books cannot be removed from your device through Calibre. Use the web interface instead.

### 3.5.2 LocalSend

1. On the reader, open **File Transfer → LocalSend** and join the same Wi-Fi network as your computer or phone.
2. Keep the receiver screen open. In the LocalSend app, select **CrossPoint** and send one or more documents.
3. Received files appear under `/received` in Browse Files. Existing names are preserved by adding a numeric suffix.

The companion `reader-send` tool in `companion/` converts public Substack and other web articles to EPUB and can deliver them through LocalSend. For subscriber-only posts, save the article or export it from your authenticated browser session and convert that local file; the companion does not collect website credentials.

### 3.6 Settings

The Settings screen allows you to configure the device's behavior. There are a few settings you can adjust:

#### 3.6.1 Display

- **Sleep Screen**: Which sleep screen to display when the device sleeps:
  
  - "Dark" (default) - The default dark Crosspoint logo sleep screen
  - "Light" - The same default sleep screen, on a white background
  - "Custom" - Custom images from the SD card; see [Sleep Screen](#37-sleep-screen) below for more information
  - "Cover" - The book cover image (Note: this is experimental and may not work as expected)
  - "None" - A blank screen
  - "Cover + Custom" - The book cover image while actively reading, falls back to "Custom" behavior otherwise
  - "Quick resume" - The text of the last page read will be displayed on the sleep screen and a moon icon is shown on the edge of the screen. Waking up the device will return to the same page of the opened book. This is useful for quickly resuming reading without waiting for the device to fully wake up and load the book.
  - "Transparent" - A transparent overlay image drawn over the current screen; see [Sleep Screen](#37-sleep-screen) below for more information
- **Sleep Screen Cover Mode**: How to display the book cover when "Cover" sleep screen is selected:
  
  - "Fit" (default) - Scale the image down to fit centered on the screen, padding with white borders as necessary
  - "Crop" - Scale the image down and crop as necessary to try to fill the screen (Note: this is experimental and may not work as expected)

- **Sleep Screen Cover Filter**: What filter will be applied to the book cover when "Cover" sleep screen is selected:
  
  - "None" (default) - The cover image will be converted to a grayscale image and displayed as it is
  - "Contrast" - The image will be displayed as a black & white image without grayscale conversion
  - "Inverted" - The image will be inverted as in white & black and will be displayed without grayscale conversion

- **Quick Resume on Timeout**: Whether to enable the "Quick Resume" sleep screen when the device goes to sleep due to inactivity (System > Time to Sleep). This is useful for quickly resuming reading without waiting for the device to fully wake up and load the book. This overwrites the Sleep Screen Cover Mode when enabled.

- **Status Bar**: Configure the status bar displayed while reading:
  
  - "None" - No status bar
  - "No Progress" - Show status bar without reading progress
  - "Full w/ Percentage" - Show status bar with book progress (as percentage)
  - "Full w/ Book Bar" - Show status bar with book progress (as bar)
  - "Book Bar Only" - Show book progress (as bar)
  - "Full w/ Chapter Bar" - Show status bar with chapter progress (as bar)

- **Hide Battery %**: Configure where to suppress the battery percentage display in the status bar; the battery icon will still be shown:
  
  - "Never" (default) - Always show battery percentage
  - "In Reader" - Show battery percentage everywhere except in reading mode
  - "Always" - Always hide battery percentage

- **Automatic screen clearing**: Reading pages choose a clearing refresh from accumulated content change instead of a fixed page interval. Each page is charged a conservative estimate of changed pixels from its ink density and the previous page's, weighted up for visible images and grayscale; the shipping firmware does not keep an exact copy of the previous frame, so this is always an estimate, never a pixel-by-pixel comparison. Initial pages, manual refreshes and required image/popup cleanup still use a clearing refresh, and a safety ceiling bounds long runs of fast refreshes; pages are not cleared every turn. By default the page after a picture is cleared, which is where most flashes in an illustrated book come from. The built-in thresholds are engineering estimates. A file at `/.crosspoint/refresh-policy.json` replaces them and can also drop the clear after pictures (see **Refresh calibration** below); delete that file and reboot to restore the built-in clearing. The manual **Refresh** action remains available. Older saved refresh-frequency values are retired without changing unrelated preferences.

- **Night mode**: Uses a dark reading background and a dark boot splash. **Dark menus with night mode** is on by default: Home, Library, Settings, dialogs and update screens follow night mode. Turn it off for light menus while keeping reading pages dark; overlays drawn directly on a reading page retain that page's polarity.

  Clearing refreshes keep the requested dark content. Manual refresh and adaptive ghost cleanup still use the controller's clean waveform. Its physical intermediate flashes can remain visible, even with a black final image; night mode does not disable ghost cleanup or promise a flash-free panel.

- **UI Theme**: Set which UI theme to use:
  
  - "Classic" - The original Crosspoint theme
  - "Lyra" - The new theme for Crosspoint featuring rounded elements and menu icons
  - "Lyra Extended" - Lyra, but displays 3 books instead of 1 on the **[Home Screen](#31-home-screen)**
  - "RoundedRaff" - A rounded theme with additional visual styling

- **Sunlight Fading Fix**: Configure whether to enable a software-fix for the issue where white X4 models may fade when used in direct sunlight:
  
  - "OFF" (default) - Disable the fix
  - "ON" - Enable the fix

#### 3.6.2 Reader

- **Reader Font Family**: Choose the font used for reading:
  
  - "Noto Serif" (default) - Google's serif font
  - "Noto Sans" - Google's sans-serif font
  - Installed SD card families

- **Reader Font Size**: Choose a point size. Built-in and direct TTF/OTF/TTC fonts offer 12, 14, 16, and 18 pt. A `.cpfont` family offers the sizes installed for that family.

- **Reader Line Spacing**: Adjust the spacing between lines; options are "Tight", "Normal" (default), or "Wide".

- **Reader Screen Margin**: **Auto** uses 20 pixels in portrait and 16 pixels in landscape. You can instead choose a fixed margin between 5 and 40 pixels in 5-pixel increments; existing fixed-margin settings are preserved.

- **Reader Paragraph Alignment**: **Book style** (the default for new settings) follows the book's explicit alignment and otherwise uses left alignment. You can also choose **Justify**, **Left**, **Center**, or **Right**.

- **Embedded Style**: Whether to use the EPUB file's embedded HTML and CSS stylisation and formatting; options are "ON" or "OFF".

- **Hyphenation**: Whether to hyphenate text in Reading Mode; options are "ON" or "OFF". Korean text wraps only at spaces when "OFF"; when "ON", a Korean word may also wrap at the end of a line between syllables or where it meets digits, Latin letters, or brackets (no hyphen is drawn).

- **Reading Orientation**: Set the screen orientation for reading EPUB files:
  
  - "Portrait" (default) - Standard portrait orientation
  - "Landscape CW" - Landscape, rotated clockwise
  - "Inverted" - Portrait, upside down
  - "Landscape CCW" - Landscape, rotated counter-clockwise

- **Extra Paragraph Spacing**: Set how to handle paragraph breaks:
  
  - "ON" - Vertical space will be added between paragraphs in Reading Mode
  - "OFF" - Paragraphs will not have vertical space added, but will have first-line indentation

- **Dictionary**: Select the StarDict dictionary used for word lookups while reading, or "None" to disable lookups. *(Only shown when at least one dictionary folder exists under `/dictionaries/` on the SD card — see [docs/dictionary.md](docs/dictionary.md) for setup and usage.)*

- **Text Anti-Aliasing**: Whether to show smooth grey edges (anti-aliasing) on text in reading mode. Note this slows down page turns slightly.

- **Images**: Whether to display embedded images (JPG/PNG) found in EPUB files; options are "ON" (default) or "OFF".

- **Focus Reading**: Bolds the first part of each word to create visual fixation points, similar to Bionic Reading. This can help improve reading speed and focus; options are "ON" or "OFF" (default).

#### 3.6.3 Controls

- **Remap Front Buttons**: A menu for customising the function of each bottom edge button.

- **Side Button Layout (reader)**: Swap the order of the up and down side buttons from "Prev/Next" (default) to "Next/Prev". You can also disable them entirely. This change is only in effect when reading.

- **Long-press Chapter Skip**: Set whether long-pressing page turn buttons skips to the next/previous chapter:
  
  - "Chapter Skip" (default) - Long-pressing skips to next/previous chapter
  - "Page Scroll" - Long-pressing scrolls a page up/down
- **Long-press Menu**: Selects the function bound to holding the menu button (Confirm) while reading an EPUB. **Cycles through the available functions** each time the setting is selected — additional functions may be added in future releases, so this is not a binary on/off toggle. A short press of Confirm always opens the reader menu as normal:
  - "Bookmark" (default) - Hold Confirm (~0.4 second) to drop a bookmark at the current page.
  - "KOSync" - Hold Confirm (~1 second) to launch KOReader sync directly.
  - "Dictionary" - Hold Confirm (~0.4 second) to start dictionary word selection on the current page (see [docs/dictionary.md](docs/dictionary.md)).
  - "Disabled" - Long-press is ignored; only short-press opens the reader menu.

- **Short Power Button Click**: Controls the effect of a short click of the power button:
  
  - "Ignore" (default) - Require a long press to turn off the device
  - "Sleep" - A short press puts the device into sleep mode
  - "Page Turn" - A short press in reading mode turns to the next page; a long press turns the device off
  - "Footnotes" - A short press in reading mode opens the footnotes submenu; if only one footnote is present on the page, the referenced page is opened directly. The short press on the power button can be used to select the footnote in the submenu, and to go back to the original page after finish reading the footnote (like the back button).
  - "Refresh" - A short press triggers a manual full-screen refresh, useful for clearing ghosting
- **Quick-return from footnotes**: Toggles on and off the quick return functionality from the footnotes. When the functionality it's active, a short press of the power button will act as the back button from the footnotes page.

#### 3.6.4 System

- **Time to Sleep**: Set the duration of inactivity before the device automatically goes to sleep; options are 1, 3, 5, 10 (default), 15 or 30 minutes.

- **Wi-Fi Networks**: Connect to Wi-Fi networks for file transfers and firmware updates.

- **KOReader Sync**: Options for setting up KOReader for syncing book progress. **Smart sync** is the default for new configurations and auto-resolves simple push/pull decisions. Existing credential files retain **Ask every time** when migrated; you can switch Sync Behavior at any time if you prefer manual confirmation.

- **OPDS Servers**: Manage one or more OPDS [(Open Publication Distribution System)](https://en.wikipedia.org/wiki/Open_Publication_Distribution_System) libraries for browsing and downloading books. See [OPDS Servers (Multiple Libraries)](#365-opds-servers-multiple-libraries) below.

- **Clear Reading Cache**: Clear the internal SD card cache.

- **Refresh calibration**: Settings → System → Refresh calibration. The reader shows its own sample pages (never your books), then a white, gray and black test card with a rung number in the corner. Photograph each card and rate the ghosting on screen. On the X4 Pro the left side key is 0 (none), a tap on the center label is 1 (faint but fine), and the right side key is 2 (bothers me); the same three words are tappable along the bottom of the card. A back swipe pauses, and the run resumes after sleep. It takes about 10 to 15 minutes. Two series show a grayscale picture page before the text pages, one with the usual clear after the picture (`ICL` in the rung number) and one without it (`IRS`). They draw the picture with the file's `image_waveform`; with `"fast"` your ratings of them decide whether the clear after pictures can go, while with the default `"quality"` the panel flashes after the picture in both series. The log is `/.crosspoint/refresh-calibration/<session>.jsonl` (log schema 2; a run started on an older build starts over). The analysis of those photos writes a `refresh-policy.json`; copying it to `/.crosspoint/` and rebooting applies it, and deleting it restores the built-in clearing. No new firmware is needed to change the thresholds.

  The file is JSON with `"schema": 1` and `"policy": "adaptive-content-v2"`, plus any of: `clear_threshold_permille` and `night_clear_threshold_permille` (how much change, in thousandths of a screen, before a clear), `safety_cap` (most fast refreshes in a row), `weight_grayscale_permille`, `weight_image_permille`, `weight_grayscale_image_extra_permille`, `frame_charge_permille`, `clear_after_image` (`true` or `false`), `image_waveform` (`"quality"` or `"fast"`) and `image_clear_region` (`true` or `false`). A value out of range is clamped. Any other key, or a file over 2 KB, makes the reader ignore the whole file and use the built-in values. With `clear_after_image` false the reader no longer adds its own clear on the page after a picture; the picture's change counts toward the next clear instead. A picture drawn with the `"quality"` waveform (the default) is still followed by one flash, because the X4 Pro panel needs it after that waveform, so on its own this key only removes the second flash when picture pages follow each other. `image_waveform` `"fast"` draws pictures with the faster gray pass and no flash after them, with fewer gray tones on pictures that cover less than about a quarter of the screen. With both keys, pictures cause no flashes of their own, and ghosts of a picture can show on the text pages after it until the next clear; run Refresh calibration with both keys in the file so its picture rungs rate exactly that. `clear_after_image` and `image_waveform` need rc9.2.1 or later: rc9.2 ignores a file that contains them.

  `image_clear_region` true (off by default; it only acts while `clear_after_image` is true) makes the clear on the page after a picture flash only where the pictures were, instead of the whole screen: the next page turns like a normal page turn, then the pictures' area alone flashes. If several picture pages come before that clear, it covers all their pictures. The whole screen still flashes when the pictures cover more than 60 % of the screen, when a picture page did not say where its pictures were, when ghosting from text reaches its threshold or the safety cap on the same page, after a manual refresh or a closing menu, on the first page, and whenever the panel cannot run a windowed clear (for example right after night mode changes). A windowed clear is not a full clear: the text's ghost budget keeps counting until the next whole-screen clear. Only X4 and X4 Pro units with the UC8279 display controller run windowed clears; other panels clear the whole screen as before. Firmware up to rc9.2.2 ignores a file that contains this key.

  **Tuning from everyday reading.** With **Record normal use** on (Settings → System → Benchmark; it is off by default), every clearing refresh in a reader adds one `refresh_clear` row to the benchmark session log in `/Benchmark/logs/`, and every refresh you ask for (the power button set to **Force refresh**, the TXT menu's **Force refresh**, or the control center's refresh tile) adds one `manual_refresh` row instead; fast page turns add no rows (see **Benchmark and normal-use recording**). On the Mac, `analyze.py --usage <card>/Benchmark/logs --ladder <calibration log> --out <dir>` reads those rows and writes a new `refresh-policy.json` and an `analysis.md`: when you refresh by hand after some fast turns, the threshold for that mode (day or night) comes down to the 25th percentile of the ghost debt at your refreshes; when 20 automatic clears pass with no refresh from you, it goes up by a quarter, but not past the most debt you accepted on the calibration ladder. Copy the file to `/.crosspoint/` as above; deleting it still restores the built-in clearing. The recorder logs everything it normally does as well, about 1.8 KB per page turn in the emulator (a 1 MiB session file holds roughly 590 turns; a new file starts at each boot or wake, and old files are never deleted), and writes to the card about every two seconds while you turn pages. Its battery cost has not been measured on the device. The log cannot see the second flash the panel makes after a picture drawn with the `"quality"` waveform, so there can be more flashes than rows.

- **Use book metadata**: Read the title and author stored inside each book when the Library index is rebuilt. When disabled or unavailable, the Library uses the filename.

- **Rebuild library index**: Rescan the SD card for books while preserving the arrival history of books already in the index.

- **Check for updates**: Check for Crosspoint firmware updates over Wi-Fi. Firmware can also be updated without a USB connection by placing a `firmware.bin` file on the SD card.

- **Language**: Set the UI language. CrossPoint supports 32 languages: English, Spanish, French, German, Czech, Brazilian Portuguese, European Portuguese, Russian, Swedish, Romanian, Catalan, Ukrainian, Belarusian, Italian, Polish, Finnish, Danish, Dutch, Turkish, Kazakh, Hungarian, Lithuanian, Slovenian, Valencian, Hebrew, Arabic, Slovak, Bosnian, Vietnamese, Norwegian Bokmål, Indonesian, and Orangutan.

- **Manage Fonts**: Browse, download, and manage custom font families installed from the SD card. See [Custom Fonts (SD Card)](#38-custom-fonts-sd-card) for more information.

#### 3.6.5 OPDS Servers (Multiple Libraries)

CrossPoint supports saving multiple OPDS servers and switching between them when browsing catalogs.

1. Open **Settings -> System -> OPDS Servers**.

2. Select **Add Server** to create a new entry, or select an existing server to edit it.

3. Configure these fields:
   
   - **Server Name**: Optional display name (for example, "Home Calibre" or "Public Catalog").
   
   - **OPDS Server URL**: Full catalog root URL (for Calibre Content Server, usually ends with `/opds`).
   
   - **Username / Password**: Optional credentials for authenticated servers.

4. Use **Delete Server** inside a server entry to remove it.

Behavior notes:

- You can store up to 8 OPDS servers.
- OPDS authentication supports HTTP Basic auth. If you use Calibre Content Server with authentication enabled, set it to Basic (not Digest).

You can also manage OPDS servers from the web interface while in File Transfer mode:

1. Connect to the device web UI.
2. Open `http://<device-ip>/settings`.
3. Use the **OPDS Servers** card to add, edit, or delete entries.

For web-based Wi-Fi network management, see [Web Settings (Wi-Fi + OPDS)](#366-web-settings-wi-fi--opds).

#### 3.6.6 Web Settings (Wi-Fi + OPDS)

While in **File Transfer** mode, the web settings page includes management cards for both **Wi-Fi Networks** and **OPDS Servers**.

1. On device: open **File Transfer** and connect through **Join a Network** or **Create Hotspot**.
2. In a browser, open `http://<device-ip>/settings` or `http://crosspoint.local`.
3. In **Wi-Fi Networks**, add, edit, or delete saved network entries (SSID + optional password).
4. In **OPDS Servers**, add, edit, or delete OPDS catalogs.

Behavior notes:

- Passwords are never shown back in the web UI after saving.
- Leaving Password blank while editing keeps the existing saved password unchanged.
- The web UI can save hidden-network SSIDs, but connecting to hidden networks still depends on the device-side Wi-Fi connection flow.
- Enterprise entries are listed/deletable in the web UI, but editing or overwriting them with PSK fields is refused. On the reader, forget and reconnect to change an enterprise login.

**Enterprise Wi-Fi (PEAP / MSCHAPv2).** Scan rows marked **Enterprise** support username/password networks, including WPA2/WPA3-Enterprise (not certificate-only Suite-B 192). Select the network, enter **Identity (username / email)**, then the password (masked). Saved enterprise entries work in every network activity and in background Reader sync. The X4 Pro radio is **2.4 GHz only**.

For **Colby Access**:

1. Obtain the current RADIUS CA from Colby ITS or the verified CA export supplied with the firmware. Copy it to `/.crosspoint/wifi-ca/colby.pem` on the SD card. This is a public certificate, not a password.
2. Ensure the reader clock is correct, using another working network to sync the clock first if needed. Certificate validity dates are checked.
3. Open **Settings → System → Wi-Fi Networks**, select **Colby Access**, enter your full Colby email as Identity, then your Colby password. Do not send these credentials to anyone.
4. Prefer **Validate server (SD CA)**. Accept the preset certificate server name `clearpass.colby.edu` and CA path `/.crosspoint/wifi-ca/colby.pem`. This exact server name was observed in Colby's EAP certificate; the IDF API checks a hostname, not a domain suffix, so `colby.edu` alone is not equivalent.
5. After connecting, choose **Yes** to save. The SD store `/.crosspoint/wifi.json` keeps identity and validation settings plus a MAC-XOR/base64-obfuscated password. This is **not encryption**: possession of the card/device can expose the login.

If ITS changes the RADIUS server or CA, obtain the new trusted settings rather than bypassing a validation failure. Missing/unreadable CA files, an incorrect hostname, an expired/untrusted certificate or a wrong clock fail closed. **Don't validate (unsafe)** is an explicit alternative; it disables server authentication. An evil-twin access point using the same SSID can then capture an MSCHAPv2 exchange and attack your college password. HTTPS validation for Reader sync remains enabled, but does not protect this earlier Wi-Fi login.

Advanced SD settings: the additive `enterpriseCredentials` array uses `type: "enterprise"`, `eap: "PEAP"`, `phase2: "MSCHAPv2"`, `ssid`, `identity`, optional `anonymous_identity`, `ca_path`, `domain` (exact certificate hostname), and `validation: "ca"` or explicitly `"none"`. Missing validation defaults to `"ca"`. Password storage uses the same `password_obf`, `password_len`, `password_crc32` fields as PSK entries; legacy plaintext imports are upgraded. Existing PSK/open entries stay in the unchanged `credentials` array, so old firmware still reads them. Old firmware cannot use enterprise entries and may discard that new array when it rewrites the file; back up the card before downgrading. At most eight saved networks total are retained.

**Connection diagnostics.** Failed attempts now distinguish network-not-found, rejection, security-handshake timeout, 802.1X rejection and an associated network that did not supply a DHCP address. Both the picker and Reader sync append one nonsecret record per attempt to `/.crosspoint/wifi-log.txt` (8 KiB, with one rotated `wifi-log.1.txt`). Records include SSID, raw ESP-IDF disconnect reason, BSSID, RSSI, channel, numeric authmode, association/IP flags and duration; unavailable channel/auth/RSSI remain `0/-1/0`, not fabricated scan data. Identities/passwords are never logged. For an open MAC-registered network such as **Colby Guest Access**, register the **MAC address displayed on the Wi-Fi screen** and use these diagnostics to distinguish association rejection from DHCP failure. A successful IP lease does not prove ClearPass authorization, captive-portal completion or internet access. The private log is not exposed through file transfer; retrieve it through USB/SD access.

After leaving a settings-only Wi-Fi picker, the station is stopped even when Home or sleep bypasses the result callback. This releases it for **Reader sync → Sync now**; the worker never takes over a live foreground transfer. Open networks also offer **Save network for next time?**, which is necessary for later unattended sync. The sync status explains a blocked request (Wi-Fi in use, foreground activity/book opening, return Home, next wake/event, no saved Wi-Fi, or retry backoff in minutes) rather than showing only “Configured; waiting.” “No saved Wi-Fi” means no stored credential, not evidence that a scan found nothing. An offline attempt is reported separately; consult `wifi-log.txt` for its actual failure.

#### 3.6.7 KOReader Sync Quick Setup

CrossPoint can sync reading progress with KOReader-compatible sync servers.
It also interoperates with KOReader apps/devices when they use the same server and credentials.

##### Option A: CrossPoint Sync Server (`sync.crosspointreader.com`, default)

When **Sync Server URL** is left empty, CrossPoint uses the free CrossPoint sync server at `https://sync.crosspointreader.com`. It speaks the standard KOReader sync protocol (so KOReader apps can use it too). CrossPoint records page starts as chapter-content offsets and sends the corresponding standard KOReader XPath, so devices with different fonts or layouts can return to the same text.

1. On each CrossPoint device:

   - Go to **Settings -> System -> KOReader Sync**.

   - Set **Username** and **Password** (enter the plain password; CrossPoint computes MD5 internally, and use the same values on all devices).

   - Leave **Sync Server URL** empty (or set it to `https://sync.crosspointreader.com`).

   - On the first device, run **Sign Up** once to create the account directly from the device. On every other device, just run **Authenticate**.

Accounts are per server. Existing `sync.koreader.rocks` credentials do not exist on the CrossPoint server; either sign up again with the same username/password or use Option B to keep using the legacy server.

##### Option B: Legacy Public KOReader Server (`sync.koreader.rocks`)

Use this if you already sync KOReader devices against the official public server.

1. On each CrossPoint device:

   - Go to **Settings -> System -> KOReader Sync**.

   - Set **Sync Server URL** to `https://sync.koreader.rocks` (required; an empty URL now points at the CrossPoint server instead).

   - Set **Username** and **Password** to your existing KOReader Sync credentials.

   - Run **Authenticate**.

2. If you do not have an account yet, run **Sign Up** on the device, or register once with curl:

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "https://sync.koreader.rocks/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

When this returns `HTTP 402` with `{"code":2002,"message":"Username is already registered."}`, pick a different username or use that existing account.

##### Option C: Self-Hosted Server (Docker Compose)

1. Start a sync server:

```bash
mkdir -p kosync-quickstart
cd kosync-quickstart

cat > compose.yaml <<'YAML'
services:
  kosync:
    image: koreader/kosync:latest
    ports:
      - "7200:7200"
      - "17200:17200"
    volumes:
      - ./data/redis:/var/lib/redis
    environment:
      - ENABLE_USER_REGISTRATION=true
    restart: unless-stopped
YAML

# Docker
docker compose up -d

# Podman (alternative)
podman compose up -d
```

> [!NOTE]
> `ENABLE_USER_REGISTRATION=true` is convenient for first setup. After creating your users, set it to `false` (or remove it) to avoid unexpected registrations.

2. Verify the server:

```bash
curl -H "Accept: application/vnd.koreader.v1+json" "http://<server-ip>:17200/healthcheck"
# Expected: {"state":"OK"}
```

3. Register a user once.
   CrossPoint authenticates against KOReader Sync (`koreader/kosync`) using an MD5 key, so register using the MD5 of your password:

> [!WARNING]
> Sending a reusable MD5-derived password over plain HTTP is insecure.
> Create unique sync-only credentials and do not reuse main account passwords.
> Prefer `https://<server-ip>:7200` whenever traffic leaves a fully trusted LAN or when using untrusted networks.
> Use `curl -k` only for self-signed certificate testing.

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "http://<server-ip>:17200/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

If this returns `HTTP 402` with `{"code":2002,"message":"Username is already registered."}`, the account already exists.

4. On each CrossPoint device:
   
   - Go to **Settings -> System -> KOReader Sync**.
   
   - Set **Username** and **Password** (enter the plain password; CrossPoint computes MD5 internally, and use the same values on all devices).
   
   - Set **Sync Server URL** to `http://<server-ip>:17200`.
   
   - Run **Authenticate**.

If you use the HTTPS listener, use `https://<server-ip>:7200` (`curl -k` only for self-signed certificate testing).

##### Syncing While Reading

Once any of the options above is set up, press **Confirm** while reading to open the reader menu, then select **Sync Progress**. Alternatively, set **Settings -> Controls -> Long-press Menu** to **KOSync** and hold Confirm to launch sync directly.

- With **Sync Behavior** set to **Ask every time**, choose **Apply Remote** to jump to remote progress or **Upload Local** to push current progress.
- With **Sync Behavior** set to **Smart sync**, CrossPoint auto-resolves simple cases: upload when no remote progress exists, confirm and leave both unchanged when local and remote progress are already synchronized, upload when local progress is further ahead, or apply remote when remote progress is further ahead.

### 3.7 Sleep Screen

The **Sleep Screen** setting controls what is displayed when the device goes to sleep:

| Mode               | Behavior                                                                                                                     |
| ------------------ | ---------------------------------------------------------------------------------------------------------------------------- |
| **Dark** (default) | The CrossPoint logo on a dark background.                                                                                    |
| **Light**          | The CrossPoint logo on a white background.                                                                                   |
| **Custom**         | A custom image from the SD card (see below). Falls back to **Dark** if no custom image is found.                             |
| **Cover**          | The cover of the currently open book. Falls back to **Dark** if no book is open.                                             |
| **Cover + Custom** | The cover of the currently open book, shown only while actively reading. Falls back to **Custom** behavior when not reading. |
| **Transparent**    | A BMP or PNG overlay drawn over the current screen. Supports PNG and 32-bit BGRA alpha transparency, and treats white as transparent in regular BMPs. Falls back to **Dark** if no valid overlay image is found. |
| **None**           | A blank screen.                                                                                                              |

With **Night mode** on, the built-in logo is dark even if **Light** is selected, and **None** is black. The entering-sleep popup keeps the current page's polarity, and Quick resume retains the page. Custom images, covers and transparent overlays keep their own colors and the explicitly selected cover filter; night mode does not automatically invert user artwork. Wake keeps the retained image until useful content is ready.

#### Cover settings

When using **Cover** or **Cover + Custom**, two additional settings apply:

- **Sleep Screen Cover Mode**: **Fit** (scale to fit, white borders) or **Crop** (scale and crop to fill the screen).
- **Sleep Screen Cover Filter**: **None** (grayscale), **Contrast** (black & white), or **Inverted** (inverted black & white).

#### Custom images

To use custom sleep images, set the sleep screen mode to **Custom** or **Cover + Custom**, then place images on the SD card:

- **Multiple Images (recommended):** Create a `.sleep` directory in the root of the SD card and place any number of `.bmp` images inside. One will be randomly selected each time the device sleeps. (A directory named `sleep` is also accepted as a fallback.)
- **Single Image:** Place a file named `sleep.bmp` in the root directory. This takes priority over the `.sleep`/`sleep` directories.

#### Transparent overlay images

To use transparent sleep overlays, set the sleep screen mode to **Transparent**, then place BMP or PNG files on the SD card:

- **Multiple Images (recommended):** Create a `.sleep-overlay` directory in the root of the SD card and place any number of valid overlay `.bmp` or `.png` images inside. One will be randomly selected each time the device sleeps. A directory named `sleep-overlay` is also accepted as a fallback.
- **Single Image:** Place `sleep-overlay.bmp` or `sleep-overlay.png` in the root directory. A root BMP takes priority over a root PNG, and both take priority over the `.sleep-overlay`/`sleep-overlay` directories.

Transparent overlay files are intentionally separate from normal sleep images. Regular BMP formats supported by CrossPoint are accepted; white pixels leave the existing screen unchanged. For per-pixel alpha transparency, use a PNG with an alpha channel or a 32-bit BGRA BMP with both visible and non-opaque pixels. Opaque white pixels in alpha images erase the content behind them.

> [!TIP]
> For best results:
> - For non-transparent **Custom** mode, use uncompressed BMP files with 24-bit color depth.
> - For **Transparent** mode, use a PNG or uncompressed 32-bit BGRA BMP for per-pixel alpha, or a regular BMP for white-as-transparent artwork.
> - X4: Use a resolution of 480x800 pixels to match the device's screen resolution.
> - X3: Use a resolution of 528x792 pixels to match the device's screen resolution.

> [!TIP]
> You can set an image as the sleep screen cover directly from the BMP image viewer in the **[Browse Files](#33-browse-files-screen)** screen.

---

### 3.8 Custom Fonts (SD Card)

CrossPoint loads additional fonts from the SD card. Custom fonts can add Chinese, Japanese, Korean, and other scripts that the built-in reader fonts lack. If your device have external RAM, you can copy `.ttf`, `.otf`, and `.ttc` files directly. Otherwise, use `.cpfont` files made from those fonts. 

There are three ways to install fonts:

1. **Download from device (recommended):** Go to **Settings -> System -> Manage Fonts**, browse the available font families, and select one to download over Wi-Fi.
2. **Upload via web interface:** While in **File Transfer** mode, open the web UI and use the **Fonts** tab to upload `.cpfont` files. The Fonts tab does not accept TTF/OTF/TTC files.
3. **Manual SD card copy:** Copy `.cpfont` families from the [crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts) to `/.fonts/` or `/fonts/`. If your device have external RAM, you can also copy TTF/OTF/TTC files there without conversion.

Once installed, custom fonts appear in **Settings → Reader → Font Family** alongside the built-in fonts.

See [docs/sd-card-fonts.md](./docs/sd-card-fonts.md) for full installation details and SD card folder structure.

---

## 4. Reading Mode

Once you have opened a book, the button layout changes to facilitate reading.

For EPUBs that group several chapters in one section, the chapter-title footer and **Contents → Current** follow the latest chapter anchor at or before the displayed page, including fragment jumps and page turns across chapter boundaries. Large EPUB metadata tables use external RAM when available; boards without it retain small internal-memory blocks, and allocation failures stop indexing rather than spilling external tables into internal RAM.

### Page Turning

| Action            | Buttons                              |
| ----------------- | ------------------------------------ |
| **Previous Page** | Press **Left** _or_ **Side Up**    |
| **Next Page**     | Press **Right** _or_ **Side Down** |

The role of the side buttons can be swapped in the **[Controls Settings](#363-controls)**.

If the **Short Power Button Click** setting is set to "Page Turn", you can also turn to the next page by briefly pressing the Power button.

### Chapter Navigation

* **Next Chapter:** Press and **hold** the **Right** (or **Side Down**) button briefly, then release.
* **Previous Chapter:** Press and **hold** the **Left** (or **Side Up**) button briefly, then release.

This feature can be disabled in the **[Controls Settings](#363-controls)** to help avoid changing chapters by mistake.

### Plain-text books (TXT)

TXT books show the first page before the full page index is built. Indexing continues in the background while reading and pauses whenever a button or the touchscreen is in use, so page turns are not held up by it; on X4 Pro, pending GT911 touch frames also interrupt indexing before the normal touch poll runs. A `~` before the page count marks an estimate. Predominantly Chinese text uses a two-character first-line indent, hides blank paragraph separators, and avoids prohibited punctuation at line starts. In that style stray whitespace from the file is cleaned for display only: the file's own indent spaces and ideographic spaces are not added to the first-line indent, a run of spaces or tabs shows as one space, a lone single space or ideographic space inside a sentence is kept, control characters, byte-order marks and zero-width spaces are not drawn, and trailing spaces never produce an empty line. Other text keeps its whitespace as written.

On X4 Pro, navigation-button contacts are captured independently of the reader loop, including while TXT indexing or EPUB idle prerendering is busy. A captured click is delivered once if it ended before the normal button poll; held buttons retain normal debounce and mapping. Power-button hold and wake behaviour are unchanged. While the device is in active use (within 3 seconds of the last input), a navigation-button edge or a touch-controller interrupt wakes the input poll at once instead of at the next 10 ms poll, and a press is accepted as soon as its 5 ms debounce has elapsed; contact bounce shorter than the debounce is still ignored, held-button repeat keeps its timing, and with no input the device polls exactly as often as before. The idle polling after 3 seconds without input, and sleep timing, are unchanged.

Press **Confirm** or tap the reading menu area for **Contents**, **Go to page**, **Go to percent**, frontlight, and refresh. Contents lists chapters found so far and marks the current one. While indexing, Go to page accepts the estimated range: selecting a page beyond the indexed portion builds up to it behind an **Indexing** progress popup. If the estimate exceeds the actual book length, the jump stops at the last page. Go to percent can open a nearby paragraph immediately without indexing the whole book; stepping back from an unindexed position may show the progress popup.

Sleep or leaving the book saves the partial index and byte position. Reopening resumes the same text and continues indexing; completed indexes are reused. Positions saved by older page-number-only firmware are translated through its saved index when available.

### Auto Page Turn

Auto Page Turn automatically advances pages at a set interval, useful for hands-free reading. This feature can be enabled and configured from the **[Reader Menu](#5-reader-menu)** while reading an EPUB.

### Tilt Page Turn (X3 only)

On the **Xteink X3**, the gyroscope can be used to turn pages by tilting the device. This feature is available in the Controls settings.

### Footnote Navigation

When reading an EPUB that contains footnotes, you can navigate to the footnote text by selecting the footnote reference in the book. From the footnote, you can return to your original reading position.

If the device goes to sleep or you close the book while viewing a footnote, the book reopens to your original reading position, not the footnote.

### Picture Viewer

On a touchscreen (X4 Pro, with touch reader controls on), a short tap on a picture in an EPUB page shows it full-screen. The picture is decoded again from the book's original image file at screen size, not enlarged from the page copy, and fit to the screen with its proportions kept: small pictures are scaled up, large ones down. A landscape picture that would come out clearly larger turned a quarter is shown sideways, its top along the left edge; turn the reader a quarter clockwise to look at it. The first view of a picture shows **Loading** while it decodes; the full-screen copy is then kept in the book's cache, so looking at it again opens without the wait.

Tap anywhere, swipe Back from the left edge, or press any button to return to the same page. The page is redrawn with a clearing refresh but not re-paginated, and your reading position does not change.

Only pictures at least 48 pixels wide and tall on the page count as tap targets, so small ornaments and inline icons keep the normal page-turn and menu taps; taps beside a picture also behave as before. A link on a picture wins over the picture. A held tap on a picture is not a picture tap: it does what it did before (with tap page turns, a chapter skip). A picture the page can only show as a placeholder box because it is too large to decode (for example a very wide PNG) shows **Picture too large to show**, and a file the reader cannot decode shows **Cannot open file**.

### Annotations

In the EPUB-based reader (including converted Markdown, FB2, and HTML), choose **Highlight / note** to capture the current page context. Choose its explicit **Type**: **Quote**, **Question**, or **Review later**, add a short note if wanted, and select **Save annotation**. The captured text is page context, not an arbitrary selection or an AI answer; the review screen labels it accordingly.

**Offline annotation and question review**

1. Open the book and choose **Reader Menu → Annotations**. Each saved row shows its explicit type; question notes are not inferred from punctuation.
2. Select a row to review the source title, chapter, full saved context/excerpt and note, original source path, chapter link, and original UTC creation time (or an explicit clock-unset label). Scroll with the usual Up/Down buttons or swipe; long text is split into bounded rows rather than silently truncated.
3. Choose **Edit annotation** to change its type/note or delete it, or **Open saved source** to leave review and return to its saved reading location. Navigation uses the native bookmark fields: spine and visible-text offset when present, otherwise the existing XPath/progress fallback. Page numbers alone are not treated as stable across font or orientation changes.
4. **Export annotations** writes the current book's annotations as Markdown under `/annotations`. An empty list is reported as empty, not as a successful export.

**Source-linked study cards (no network or AI)**

1. Select an already saved annotation, then **Create study card**. The firmware reads the persisted annotation and saves a local snapshot; unsaved editor changes are not used.
2. The new card opens on its **question/front**. For a **Question** annotation with a note, that note is the front. Otherwise the firmware uses a fixed recall question appropriate to Quote, Question, or Review later. It does not invent a factual answer.
3. Choose **Reveal answer** to show the saved context/excerpt and note; **Hide answer** returns to the question-only side. Source title/chapter/type remain visible on both sides. **Open saved source** returns to the original reading location from either side.
4. Reopen the book's deck with **Annotations → Study cards**, including after reboot, and select any card to review it again offline. **Delete study card** requires confirmation and does not delete its annotation.

Cards preserve the complete native persisted annotation snapshot: annotation ID, explicit kind, original source path/title, chapter title/href, spine, XPath, cached page/page-count/progress, optional visible-text offset, excerpt, note, page-context flag, and UTC creation time. Editing or deleting the original annotation does not change an existing card; delete and recreate the card to refresh it. Repeated creation makes separate cards.

Limits: 128 annotations and 128 cards per book; 768-byte captured context and 256-byte notes/fronts (UTF-8-safe truncation at capture). Cards live in `/.crosspoint/study/<source-key>/`, separately from `/.crosspoint/annotations/`. Back up both directories along with the original documents. This is a book-local, manually selected review deck, not spaced repetition, grading, a whole-library card index, or companion/cloud card sync. Deck opening reads at most that book's bounded card records once; selection/reveal renders from RAM without rescanning the SD card. Empty decks, unreadable/corrupt records, save failures, and missing source files have explicit messages. A damaged record blocks that deck rather than hiding the damage as an empty or partially successful list. Restore a known-good backup to repair it; the firmware does not silently discard damaged records.

Original files must remain at their saved paths. Renaming/moving a source changes its annotation/deck identity; replacing or substantially editing a source can invalidate saved anchors. Exact-offset navigation targets the saved text position in an unchanged source after repagination; older annotations without offsets use the reader's less precise XPath/progress fallback. This workflow has not been physically verified on an X4 Pro merely by compiling or running host/emulator checks.

### Offline Dictionary

Words on the current page can be looked up in an offline StarDict dictionary stored on the SD card. Copy a dictionary to the `/dictionaries/` folder, select it in **Settings → Reader → Dictionary**, then start a lookup by choosing **Look Up** in the **[Reader Menu](#5-reader-menu)** (or by holding **Confirm**, if the **Long-press Menu** setting in **[Controls Settings](#363-controls)** is set to "Dictionary"). Use **Left/Right** to highlight a word and press **Confirm** to show its definition.

On a touch screen you can also **press and hold a word** in an EPUB for about half a second: its definition opens directly, and **Back** returns to the same page. Holding blank space does nothing. If **Long-press Chapter Skip** (or rotate on hold) is set and page turns use tap zones, holding those zones still skips or rotates; the lookup takes holds everywhere else. TXT books do not have dictionary lookup.

For Chinese novels, install the **CC-CEDICT Chinese-English** folder and select `cedict` in the Dictionary setting. In Look Up mode, tap the **first character** of a word (or move the highlight with the direction buttons and press Confirm); while reading, press and hold the first character. The reader checks up to **eight consecutive CJK characters** and opens the longest exact dictionary match. The definition begins with the matched Chinese headword, tone-mark pinyin and English glosses. Tap **Shorter / Longer** at the top of the definition, or press the side **Up / Down** buttons, to move between available matching lengths. Definition paging and Back work as before.

Matching stops at punctuation, non-CJK text and the **end of the visible line**; it does not join a word split across two lines or pages. Choose a different layout/font size if that word is split. Both simplified and traditional headwords are indexed. English lookup keeps its existing exact-match, synonym and stemming behavior; select an English dictionary when reading English. No dictionary index is loaded into RAM. The first lookup creates the usual small on-card `.qidx` sidecar.

Generate the redistributable dictionary with `python3 companion/scripts/build_cedict.py OUTPUT/cedict` (see the companion README). Copy the whole `cedict` folder into `/dictionaries/`, including `LICENSE.txt` and `ATTRIBUTION.txt`. CC-CEDICT and this converted database are licensed **CC BY-SA 4.0**.

See [docs/dictionary.md](docs/dictionary.md) for supported formats, setup, and where to find dictionaries.

### System Navigation

* **Return to Home:** Press the **Back** button to close the book and return to the **[Home](#31-home-screen)** screen.
* **Return to Browse Files:** Press and hold the **Back** button to close the book and return to the **[Browse Files](#33-browse-files-screen)** screen.
* **Reader Menu:** Press **Confirm** to open the **[Reader Menu](#5-reader-menu)**, which includes chapter navigation, reading options, and more.
* **Long-press Confirm (configurable):** Holding **Confirm** runs the function chosen by the **Long-press Menu** setting in **[Controls Settings](#363-controls)** — "Bookmark" (default) drops a bookmark, "KOSync" launches KOReader Sync, "Dictionary" starts a word lookup, "Disabled" does nothing. A short press always opens the Reader Menu.

### Supported Languages

CrossPoint renders text using the following Unicode character blocks, enabling support for a wide range of languages:

* **Latin Script (Basic, Supplement, Extended-A/B):** Covers English, German, French, Spanish, Portuguese, Italian, Dutch, Swedish, Norwegian, Danish, Finnish, Polish, Czech, Hungarian, Romanian, Slovak, Slovenian, Turkish, Catalan, and others.
* **Cyrillic Script (Standard and Extended):** Covers Russian, Ukrainian, Belarusian, Bulgarian, Serbian, Macedonian, Kazakh, Kyrgyz, Mongolian, and others.
* **Vietnamese:** Supported via extended Latin glyph coverage in the built-in reader fonts.

The UI includes Arabic and Hebrew (menus use built-in fonts with presentation-form coverage). Built-in **reader** fonts do not cover Chinese, Japanese, Korean, Arabic, Greek, Hebrew, or Farsi for book text. **CJK, Hebrew, Arabic, Greek, and other extended scripts can be enabled for reading by installing custom SD card fonts** — see [Custom Fonts (SD Card)](#38-custom-fonts-sd-card).

---

## 5. Reader Menu

Press **Confirm** while reading to open the Reader Menu. From here you can access reading utilities and navigation options without leaving the book.

Available options include:

- **Select Chapter** – Open the table of contents to jump to a specific chapter (see [Chapter Selection](#51-chapter-selection) below).
- **Reading stats** – Active reading time, pages, sessions, average pace and estimated time left for this book; available in EPUB, TXT and XTC menus.
- **Footnotes** – Navigate to the footnotes for the current section *(only shown in books that contain footnotes)*.
- **Look Up** – Select a word on the current page and show its dictionary definition (see [docs/dictionary.md](docs/dictionary.md)). Requires a dictionary to be selected in **Settings → Reader → Dictionary**. On a touch screen, pressing and holding a word on the page looks it up without the menu.
- **Reading Orientation** – Cycle through screen orientations without leaving the reader.
- **Auto Turn (Pages Per Minute)** – Cycle through automatic page turn speed options for hands-free reading.
- **Go to %** – Jump to a specific position in the book by percentage.
- **Take screenshot** – Save a screenshot of the current page to the `screenshots/` folder.
- **Show page as QR** – Display a QR code encoding the current reading position.
- **Go Home** – Close the book and return to the Home screen.
- **Sync Progress** – Push or pull reading progress with a KOReader sync server (see [KOReader Sync Quick Setup](#367-koreader-sync-quick-setup)).
- **Delete Book Cache** – Clear the cached layout data for the current book, forcing a re-index on next open.

Press **Back** at any time to close the menu and return to your current page.

### 5.1 Chapter Selection

Accessible by selecting **Chapters** from the Reader Menu.

1. Use **Left** (or **Side Up**), or **Right** (or **Side Down**) to highlight the desired chapter.
2. Press **Confirm** to jump to that chapter.
3. *Alternatively, press **Back** to cancel and return to your current page.*

---

### 5.2 Bookmarks

Bookmarks can be created to quickly save and restore your place in a book.

To create a bookmark, hold **Confirm** for about half a second while inside a book. A popup will appear letting you know a bookmark was created. The popup message will automatically disappear in a couple of seconds.

To open bookmarks, press **Confirm** while inside a book. Then navigate to the **Bookmarks** menu. Bookmarks can be opened by navigating to them and pressing **Confirm**, which will redirect you to that place in the book. You can delete bookmarks by holding **Confirm** for about 0.7 seconds, and then pressing **Confirm** again to confirm deletion, or **Back** to cancel.

Bookmarks are stored in the `.crosspoint/bookmarks` folder in the JSON format.

### 5.3 Reading statistics

Open **Reader menu → Reading stats** for the current book, or **Settings → System → Reading stats** for today's total, the last 7 and 30 local-calendar days, and the ten most-read books by all-time active time. The book screen also links to **Reading overview**. Durations use `hours:minutes:seconds`.

Time starts after the first page has rendered. Only the foreground, awake reader earns time, and credit stops five minutes after the last button or touch input. Another input resumes credit without backfilling the idle interval. Opening menus, dictionaries or the light panel pauses credit. Returning to the reader continues the same session; reopening a book or waking into it starts a new session. Automatic benchmark fixtures do not contribute.

Pages count completed changes of displayed page, in either direction; a chapter/percentage jump counts as one displayed-page change, not every skipped page. Progress advanced sums forward movement in hundredths of a percent (EPUB content-position estimate, TXT byte-position estimate, XTC page position). It is not a count of characters read; rereading and jumps can exceed 100%. Average pace uses active time per page change. Time left uses active time divided by forward progress and is unavailable until at least one minute and some forward progress have been recorded. It is an estimate, especially after jumps, rereading or layout changes.

First/last dates and daily buckets use the configured local clock. If it is unavailable or unset, book totals still accumulate, dates show `--`, and the overview asks you to set the clock; undated time is not retroactively assigned to a day. Renaming or moving a book starts a separate record because identity is its full SD path.

Statistics live privately under `/.crosspoint/reading-stats/`: one small book record per path plus a fixed-size daily/top-books summary. Version 1 records have a checksum and two alternating generations; each new generation is written and synced to a temporary file, then renamed. Startup chooses the newest valid generation, falls back to the previous one after corruption/interruption, or starts empty if both are invalid. The format is independent of rebuildable book-layout caches.

There are no statistics writes on individual page turns. Dirty totals are checkpointed every three minutes and on covering/leaving the reader, including sleep. Continuous active reading therefore makes at most **20 periodic checkpoints / 40 logical record commits per hour**, plus lifecycle checkpoints (two record commits each). Filesystem metadata and SD-controller physical writes are additional, not measured by that count. An interrupted save can lose up to the checkpoint interval; book and summary records are independently recoverable, so their latest totals can differ by one checkpoint after interruption. A failed save is logged and retried at the next checkpoint rather than every loop.

When a benchmark session is recording, every flush that reaches an open book (periodic checkpoint, cover/suspend, leaving the reader, direct call) is written as one `stats_flush` row whose span is the whole call, so the SD cost of a checkpoint can be placed against the turn it landed near. The row changes nothing about when or how statistics are saved; with no session it costs one inlined check. Details in the event table below.

## 6. Current Limitations & Roadmap

Please note that this firmware is currently in active development. The following features are **not yet supported** but are planned for future updates:

* **Cover Images:** Large cover images embedded into EPUB require several seconds (~10s for ~2000 pixel tall image) to convert for sleep screen and home screen thumbnail. Consider optimizing the EPUB with e.g. https://github.com/bigbag/epub-to-xtc-converter to speed this up.
* **Unsupported Image Formats:** Most JPG and PNG images in EPUBs render correctly. GIFs and progressive JPEGs are not supported and will fall back to an `[Image]` placeholder.

---

## 7. Troubleshooting Issues & Escaping Bootloop

If an issue or crash is encountered while using Crosspoint, feel free to raise an issue ticket and attach the logs.

**Crash reports on SD card:** After a crash, CrossPoint automatically saves a crash report to the SD card (no USB connection needed). Check the root of the SD card for a crash log file and include it with any bug report.

### Benchmark and normal-use recording

Open **Settings → System → Benchmark**. No serial connection, marker file, copied test book, or manual page-turn checklist is needed. Recording is off by default and independent of the firmware's serial log level.

- **Run benchmark** prepares built-in EPUB, TXT and BMP fixtures in the private `/.crosspoint/benchmark/fixtures-v1/` directory, resets only those EPUB/TXT caches, and opens the actual readers. It completes **10 EPUB forward turns**, reopens the EPUB with warm caches for **3 more turns**, completes **5 TXT forward turns**, and paints the BMP. Each turn waits for a newly completed, advanced page followed by about two seconds of visible dwell; indexing and error screens do not count. Each reading open starts at the beginning rather than loading fixture progress.
- The screen returns to the Benchmark menu with the outcome, actual completed counts and a unique `/Benchmark/logs/session-….jsonl` path. **Finished; result saved** appears only after the workload and log footer/sync/close all succeed. A failed or cancelled run is not a successful benchmark. **Not finalized** means that the file may contain useful preceding rows but must not be treated as a complete result.
- **Back, Home or power** cancels the run; Home leaves the menu, and normal manual sleep still works. If indexing or a render is already in progress, cancellation is remembered but navigation waits for that work to finish; no further automatic page turns are issued. Automatic sleep is inhibited during the run. A missing completed page times out after two minutes; a loading/render failure, lack of forward progress, fixture conflict, or recorder error is reported instead of advancing the workload. A conflicting private fixture is never overwritten. If the selected reader font is not loaded, the test reports that before preparing files or starting a session; it does not silently benchmark a built-in fallback or rewrite your font preference.
- **Record normal use: On/Off** is persisted and starts/stops recording immediately, including without a reboot. This control is owned by the on-device Benchmark submenu and is not exposed or editable through the web settings API. When On, later boots/wakes also start a fresh normal session. The recording subtitle shows whether a session is actually active; after an error or file limit it stays stopped until you toggle Off then On or boot again. A later failure of resumed normal recording does not replace an already saved automatic result.
- Running the automatic test closes any normal-use session, creates a separate automatic session, and resumes normal recording afterward if it was enabled/active. The displayed automatic result path is retained even when the resumed normal session creates another file. The run does not change your book files, current book, recents, reading progress or reader settings, and it never clears unrelated reading caches.
- **Run navigation benchmark** is a separate row that presses real buttons through a script of named steps and logs, per step, injection-to-first-refresh, injection-to-settled, refresh type and heap minimum. It is selectable alongside the page-turn run and never changes it. See **Navigation run** below.
- Normal sleep, restart and entering **File transfer → USB drive** flush and close recording before SD ownership changes. Retrieve logs from `Benchmark/logs` using a card reader or the existing file-transfer workflow, and safely eject storage. Logs are never overwritten or automatically deleted; remove collected logs manually if desired.

Each session has a schema **3** `session` row with `mode` (`normal` or `automatic`), compiled version, build time/date, and the **application ELF SHA-256** (not a hash of the packaged firmware `.bin`), plus ESP reset/wake causes and the HAL wake classification. `refresh_policy` identifies `adaptive-content-v2`; `refresh_policy_version` is 2, `refresh_policy_source` is `compiled` unless `/.crosspoint/refresh-policy.json` is in force, `refresh_policy_config` lists the seven numeric tunables in effect, and `refresh_policy_clear_after_image`, `refresh_policy_image_waveform` and `refresh_policy_image_clear_region` record the three picture settings. Schema 2 recordings instead used a fixed refresh-page count. The `hardware` row records runtime board, controller enum, controller variant and panel-probe verdict/validity. A profile-default controller with an inconclusive or invalid probe is not proof of the physical controller. Older `/Benchmark/RECORD` files have no effect.

**Measurements and limits**

- Records include activity entry, completed render-call spans for EPUB/TXT and the synchronous BMP viewer, display API calls and BUSY-wait calls, selected settings snapshots at entry/render when changed, internal/PSRAM free/total/minimum/largest-block bytes, existing EPUB/library indexing intervals, reader-only input activity flags, battery voltage/percent samples, one summary row per Reader sync run (trigger, duration, transferred byte counts, battery before and after), one row per clearing or manual refresh in a reader (cause, fast turns, ghost debt, picture/grayscale page counts, night and policy-file flags, book ID), and sleep/session end. No book text, document paths, credentials, network payloads (a sync run records only how many bytes moved, never what they were), key identities or touch coordinates are recorded. Activity, BUSY-tag, SD-font and book identifiers use stable FNV-1a 32-bit hashes; these are identifiers, not cryptographic anonymization.
- `start_us`, `end_us` and `duration_us` use the monotonic ESP timer **within one boot**, not wall-clock time. EPUB phases and index intervals retain their existing `millis()` start/end values at 1 ms resolution; their microsecond start is reconstructed from that duration and the observation time. Do not compare monotonic timestamps across session files.
- `render` measures a render call, not button-to-page latency; asynchronous submission and subsequent wait are separate display events. BUSY measurements are the **software wait-call interval**, including its polling/grace delays and power hooks, not exact electrical edge widths or optical settling. `index` is combined parsing/cache/indexing work, **not raw SD latency**.
- The fixed RAM queue holds 64 events. Overflow drops new events, preserves queued events, increments the cumulative `loss` count and leaves gaps in positive sequence numbers. Each session file is capped at 1 MiB; a reserved footer records the limit and discarded tail count, then recording stops until an explicit new session or boot. Logs are never automatically deleted or rotated over older sessions.
- The main loop attempts a flush every two seconds only after actually acquiring the render lock, so an active render postpones it. `flush` reports the preceding batch's write-and-sync time and bytes. That measurement excludes persistence of the observer row itself and final close; recording still adds RAM/callback/SD overhead and is not a zero-overhead benchmark.
- Open/write/sync/close failure disables that session without stopping normal reading. Keep complete preceding JSON lines if the final row is truncated. Normal terminal paths close the file before SD unmount/raw USB ownership. Sudden power loss, panic, removal, or a failing card may lose the buffered tail or leave filesystem damage: this is **not a crash-safe journal**. A missing `end` row means the session was not cleanly finalized.
- **Battery sampling.** While a session is recording, the main loop reads the battery about **every 5 minutes while awake** (`battery_sample`), plus one reading when each session starts and one at deep-sleep entry (after any pre-sleep sync, before the SD log closes). Nothing is sampled while the CPU sleeps, so the drain across a sleep is the difference between its sleep-entry sample and the next session's first sample; compare `millivolts` rather than `percent` where precision matters, and use the flags to discard samples taken while charging. A sample costs one fuel-gauge read, is skipped while recording is off, and is not taken during **File transfer → USB drive**. It does not change the smoothed battery percentage shown in the UI.

**Schema 3 event data**

Every timed event has `event`, `seq`, `start_us`, `end_us`, `duration_us`, and an eight-element numeric `data` array. Unused trailing slots are zero. `seq=0` identifies flush/loss/footer records generated by the writer, not the producer sequence. Units and slot meanings:

| Event | `data` slots, in order |
| --- | --- |
| `activity` | Activity-name ID, is-reader flag. Common names to hash: `EpubReader`, `TxtReader`, `BmpViewer`, `Home`, `Sleep`. |
| `render` | Activity-name ID, activity healthy-render flag (BMP: this image completed), source: 0 ordinary render task / 1 reader render task / 2 synchronous viewer. A reader's healthy flag may remain true after its first good frame; it is not per-page error certification. |
| `display_call` | Operation: 0 buffer / 1 async submit / 2 async wait / 3 refresh / 4 grayscale base / 5 grayscale display / 6 precondition / 7 display sleep / 8 configured grayscale base / 9 region clear; requested mode enum where applicable. Operation 8 uses a grayscale-mode enum in slot 1 and fallback refresh-mode enum in slot 2; operation 4 uses the refresh-mode enum in slot 1; operation 9 packs the cleared panel-memory rect as x × 65536 + y in slot 1 and width × 65536 + height in slot 2. |
| `busy_wait` | Driver-tag ID, method: 0 poll / 1 interrupt; outcome: 0 idle observed / 1 timeout / 2 assertion not observed. Poll fallback excludes any grace period performed by its caller before entering the poll; the encompassing display call includes it. |
| `memory` | Internal free, total, minimum-ever free, largest block; then the same four PSRAM counters; all **bytes** (zero when PSRAM is absent). |
| `index` | Kind: 0 EPUB spine/TOC/cache build / 1 complete library build; start `millis()`, end `millis()`, success flag. Library subphases are 2 prepare/prior lookup, 3 directory walk/staging, 4 reconciliation, 5 title ordering, 6 author ordering/emission. Slot 3 is source parsing/checkpoint time in milliseconds for kind 3, reserved (zero) for kinds 2/4/5, and emission success for kind 6. Subphases overlap the complete-build interval; do not sum them with kind 1. Cached EPUB loads do not emit a new index event. |
| `epub_phase` | Phase enum, start `millis()`, end `millis()`, detail. Phases: 0 prewarm, 1 BW render, 2 display, 3 gray render, 4 wait, 5 gray write, 6 gray display, 7 cleanup, 8 BW store, 9 gray LSB, 10 gray MSB, 11 BW restore, 12 total, 13 idle prerender. Branch-specific phases can overlap or include other work; do not sum all phase/total rows. Detail (slot 3): on phases 1 and 12 it is 1 when that page was presented from an idle-prerendered body and 0 when it was rendered in full (always 0 before the prerender existed); on phase 13 it is the prerender outcome: 1 built (B/W body), 2 built (B/W body plus both text-grayscale planes), 3 abandoned because input arrived, 4 skipped for lack of spare PSRAM, 5 skipped as ineligible (image page, a grayscale route that is not buffered, or a next section without a complete cache). Phase 13 spans one idle build, is emitted while the screen is idle and sits inside no `render` span. |
| `reader_input` | Any button pressed, any button released, touch activity flags, observed after input polling while a reader activity is current. No event is consumed or automatically paired to a render. |
| `sleep_request` | Automatic-timeout flag. The final `end` row also covers early boot paths returning to sleep. |
| `reader_settings` | Font-family enum, font point size, spacing enum, orientation enum, adaptive refresh-policy version (2), text-AA flag, inverted flag, margin setting (0 means automatic). In schema 2 only, slot 4 was the fixed full-refresh frequency in pages; do not interpret version 2 as a one-page interval. |
| `display_settings` | Sleep-screen enum, SD-font-name ID. Paired with `reader_settings` at the same timestamp; snapshots are emitted only when changed. |
| `loss` | Cumulative queue-overflow count. |
| `flush` | Batch bytes written before its sync completed, cumulative file bytes before this observer row. |
| `run_step` | Phase: 1 EPUB first open / 2 EPUB reopen / 3 TXT / 4 BMP; completed turns (BMP: completed paint); target count; state: 8 newly completed page / 9 phase start, otherwise terminal outcome below. The initial reader page has count 0 and is not a forward turn. |
| `run_result` | Outcome: 0 complete / 1 cancelled / 2 loading failed / 3 rendering failed / 4 no progress / 5 timeout / 6 recorder unavailable; actual EPUB-first, EPUB-reopen, TXT and BMP counts; last phase; elapsed milliseconds. A complete result also requires the successful `end` footer; a cap/write failure may prevent the summary itself from being written. |
| `sync_run` | Trigger: 1 manual / 2 wake / 3 pre-sleep / 4 charging / 5 interval / 6 continue; status: Reader-sync phase code in the low byte (0 unconfigured, 1 disabled, 2 waiting, 3 connecting, 4 syncing, 5 up to date, 6 offline, 7 invalid config, 8 clock error, 9 memory error, 10 storage error, 11 protocol error, 12 auth error, 13 rate limited, 14 needs re-delivery, 15 annotation error, 16 cancelled) plus `256` if the radio was **not** confirmed off; HTTP response-body bytes received; request-body bytes sent (headers and TLS framing excluded); battery millivolts before, after; battery percent before, after. The span is the worker's run from start to radio off. Its `end_us` is reconstructed from the worker's finish time, because the row is written by the main loop shortly afterwards. "Before" is read just before the radio starts and "after" once it is off; an unreadable value is 0. |
| `battery_sample` | Battery millivolts, battery percent (0 when unreadable); reason: 0 periodic / 1 session start / 2 deep-sleep entry; flags: bit 0 the charger reports charging, bit 1 USB power is present. An instant event (`start_us` = `end_us`). |
| `refresh_clear` | Cause: 0 unlabelled (the reader cleared although the policy chose fast) / 1 initial page / 2 ghost debt reached the threshold / 3 safety cap / 4 page after a picture / 5 indexing popup / 7 closing overlay (option popup, night-mode toggle) / 8 polarity change / 9 unreadable frame metrics / 10 other reader request; fast refreshes since the previous clear; debt at this clear in thousandths of a screen (the page's own change included); how many of those fast pages had a picture; how many had a visible grayscale pass; flags: bit 0 night (inverted) output, bit 1 `/.crosspoint/refresh-policy.json` in force, bit 2 this page has a picture, bit 3 this page has a visible grayscale pass, bit 4 a region clear (`image_clear_region`: only the `region` rect flashed, and the turn and debt counts carry on to the next whole-screen clear), bits 5–7 why a page-after-a-picture clear in region mode still covered the whole screen (0 it did not / 1 a picture page gave no area / 2 the area was off screen / 3 over 60 % of the screen / 4 the panel could not run a windowed clear / 5 ghost debt, the safety cap or another stronger reason needed the whole screen); book ID (FNV-1a of the book's path); the clear threshold in force for this polarity, in thousandths. After `data`, `region` is the logical rect `[x, y, width, height]` a region clear covered, all zero for a whole-screen clear. One instant row (`start_us` = `end_us`) per clearing refresh a reader commits; fast refreshes write nothing. |
| `manual_refresh` | The same eight slots, cause 6: a refresh you asked for (power button set to **Force refresh**, the TXT menu's **Force refresh**, or the control center's refresh tile). It replaces the `refresh_clear` row for that clear, so every clear is exactly one of the two. |
| `end` | Reason: 0 sleep / 1 USB drive / 2 restart / 3 file limit / 4 user toggle or normal-session pause / 5 automatic complete / 6 automatic cancelled / 7 automatic failure; cumulative queue losses; queued events discarded at the limit; file bytes before the footer. |

**Schema note.** The session schema stays **3**. `sync_run` and `battery_sample` were appended to the event-name table as additive events: every existing row, slot meaning and name is unchanged, so a schema-3 reader that skips unknown `event` names keeps working, and logs recorded before this change simply contain neither event. A strict reader that rejects unknown names must learn these two. Names and the trigger/reason/phase codes in their slots are append-only. `data` carries integers only: no server name, path, SSID or content.

**Idle prerender note (additive within schema 3).** The EPUB reader now renders the page after the one on screen into spare PSRAM once the screen has been idle for about 0.4 s, and presents that body when a forward turn lands on exactly that page; the status bar is always drawn fresh. It is built only under the render lock, is abandoned when any button or touch input is seen (so it never delays a turn), is skipped unless a healthy PSRAM reserve stays free, and is dropped on any layout, font, orientation, anti-aliasing, night-mode, book or menu change. A next-section page is prerendered only when that section's cache already exists and is complete; sections are never built in the background. Schema 3 itself is unchanged: `epub_phase` gains phase code 13 and meaning for slot 3 on phases 1, 12 and 13 (above); no existing row, slot or name changes meaning, and a reader that ignores unknown phase codes keeps working. To judge it, compare `epub_phase` total (12) spans with detail 1 against those with detail 0, divide the detail-1 count by all phase-12 rows for the hit rate, and read the phase-13 rows for built, abandoned and skipped counts; the `memory` rows show whether the PSRAM minimum stayed above the reserve. On the measured device a turn is dominated by the display refresh, so only the non-display CPU part can shrink.

**Refresh rows note (additive within schema 3).** `refresh_clear` and `manual_refresh` were appended to the event-name table (after `battery_sample`); no existing row, slot or name changes meaning, and a reader that skips unknown `event` names keeps working. They are queued from the reader's render path only when a clearing refresh is committed, through the same 64-event queue, flush, 1 MiB file limit and footer as every other row, so they add no SD write path of their own, and a session that is not recording costs one flag check per clear. Calibration runs (Settings → System → Refresh calibration) do not write them. The book ID is a hash of the book's path, an identifier only: neither the path nor the title is recorded. The `region` key, flag bits 4–7 and `display_call` operation 9 came later and are additive too: a region-clear row is not a whole-screen clear, so tools that time runs between clears (`analyze.py --usage`) skip rows with flag bit 4.

**Navigation run (additive within schema 3).** `nav_step`, `nav_result`, `nav_input`, `nav_rows` and `nav_restore` extend the event-name table after `manual_refresh`, followed by a reserved slot (retired: a temporary Wi-Fi teardown timing probe used it, nothing writes it now) and `stats_flush`. The RC9.3 refresh events retain their slots; existing event names and data meanings are unchanged. A strict reader that rejects unknown names must learn these six navigation/statistics events. Step ids, statuses and outcome codes are append-only.

The script is text, one step per line (`#` comments), parsed lazily with no heap use: `<step-id> guard=<Activity|any> [lead=<keys>] [key=<Key[@holdMs]>] [x=<reps>] [do=<verb>:<arg>] [expect=<Activity>] [needs=<feature>] [quiet=<ms>] [norefresh=<ms>] [timeout=<ms>] [setup] [soft] [optional] [external] [blind] [sync]`. Keys are `Back Confirm Left Right Up Down PageForward PageBack`, mapped to physical buttons through your button layout exactly as the activities map them; a key your layout cannot produce (side buttons disabled) stops the run (`unmappable`) instead of pressing something else. `lead` is a comma list of unmeasured presses (`Down*3` repeats), each waiting for the screen to settle. Defaults: hold 60 ms, `quiet` 700 ms without a refresh before the screen counts as settled, `norefresh` 2500 ms with no refresh at all before "settled without refresh", `timeout` 120 s. Slow screens (a book open, indexing) must raise `norefresh`/`quiet` or use `expect`: a step with `expect` is not settled until that activity is current. A script holds at most 400 steps, 16 lead presses, 40-character ids, and the SD file at most 24576 bytes (larger is rejected from its size before any read, never truncated). The script buffer is allocated only when the file exists, sized to the file plus a terminator, and freed when the run ends: a run with no script on the card allocates nothing. The `BENCH` log line `nav script N bytes; internal free A -> B, largest block C -> D` records the transient internal-heap cost of the buffer on each run (read it from the serial log or the session log of your own device; no figure is claimed here because none was measured on hardware). Two steps may share an id only when each has its own `needs=` (variants of one step for different prerequisites).

- **Software actions (`do=`).** A step has either a `key` or a `do` action, never both and never with `external`. `do=open:<absolute path>` opens that book the way the Library, Files and Home screens do (the step fails the run if the file is missing; no `..`); `do=dict:<folder>` selects `/dictionaries/<folder>` and `do=menu:list|toolbar` selects the reader menu style. Both settings are changed in RAM only: nothing is saved, and the values captured before the run are put back after it. `do=goto:library` opens the Library directly (the Home row for it depends on the theme and on the number of recent books), `do=goto:home` opens a fresh Home with the Settings row selected (the state a Back out of Settings leaves) and clears the whole activity stack, so a script can start from any Settings tab or cursor position without counting Back presses, and `do=idle:<label>` presses nothing: the step is a timed wait, used for the TXT background index. Everything else in a script is a real button press.
- **`blind` steps and the Unverified status.** The toolbar reader menu, its Contents/Text/More panels and a TXT index wait are overlays or background work inside one activity (`EpubReader`, `TxtReader`), so no `guard`/`expect` can prove which screen the press reached. A step marked `blind` is still timed (first refresh, settled) but, if it settles, it is recorded as **Unverified** (status 7) and counted in its own column, never as ok or no-refresh. A blind step that does not settle still fails the run, and the next step's guard or `expect` is the only check on where the press landed. Unverified is not a pass: a run whose steps are mostly Unverified has timings, not proof of the screen.

- **Guards fail, they do not skip.** A step whose `guard` activity is not current records `diverged`, nothing is injected and the run ends with outcome *diverged*; the log footer is then a failure, never a completed run. A mismatching `expect` ends the run the same way unless `soft`. Only `optional` steps (a prerequisite that may legitimately be absent) are skipped as *unsupported*, and `needs=<feature>` steps whose feature is unavailable are *skipped*. Features the device reports: `recent`, `norecent` (recent-book row present or not), `opds`, `noopds` (an OPDS server is configured, which adds a Home row), `dictionary` (a dictionary is selected), `touch`, `stats`, `darkmenus`, `night`, `wifiui`, `cjklookup` (firmware features, always present in this build), `syncsafe` (no `/.crosspoint/wifi.json`, so a Sync now press cannot reach any network; a reader with a saved network skips those steps rather than contact a server), and fixtures on the card: `fixture` (`/.crosspoint/nav-fixtures/navigation.epub`), `probebook`, `bigepub`, `groupbook` (`/navfeat/probebook.epub`, `spinebook.epub`, `groupbook.epub`), `txt` (`/navfeat/novelbook.txt`), `navdict` (`/dictionaries/navigation`), `cedict` (`/dictionaries/cedict/cedict.ifo`), the composites `dictfix` (probebook and navdict) and `cedictfix` (probebook and cedict) that skip a whole dictionary segment together, and `readerfix`/`noreaderfix` (any book fixture is on the card or none). Skipped, unsupported, unverified and `external` counts are always shown next to the result and are never counted as passed: a run with skipped fixture steps is not full coverage, and the skipped count also includes the unused variant of every step that has `needs=` variants. `external` steps (sleep, wake, a real sync against a server) cannot be self-driven (nothing can wake the reader, and a real sync changes server state) and are recorded as such. A `needs=` the build does not know is unavailable, never passed.
- **Same script, two adapters.** The step ids are semantic (`home.library.open`, `settings.move.0`, ...) and shared with the emulator runner; the device adapter presses physical buttons, the emulator adapter touches. Coordinates and key choices need not match, ids and ordering do.
- **Built-in script** (used when `/.crosspoint/benchmark/nav-script.txt` does not exist). It starts from the Benchmark screen with one setup `do=goto:home` step to a fresh Home (no Back presses, so it does not depend on the Settings tab or cursor the run was started from), then moves over Home (`home.move.0..3`), opens and leaves Library (`home.library.open`, `library.move.*`, `home.library.back`), Files (`home.files.open`, `files.move.*`, `home.files.back`) and Settings (`home.settings.open`, `settings.move.0..3`, `home.settings.back`), and ends with the external `sleep.home`/`wake.home` rows. It is non-mutating: no book is opened, no setting toggled, nothing deleted; Settings saves its unchanged values when Back leaves it. Home reopens with the row of the screen it came from selected and lists the recent books first (up to three on the cover theme), then Library, Reading Inbox, Files, [OPDS], File Transfer and Settings, so every jump is counted from that known row toward the end of the list (`lead=Up*4` for Library, `Down*2` for Files and Settings) and does not depend on the number of recents; a configured OPDS server adds a row, hence the `.opds` variants of Library and Settings selected by `needs=opds|noopds`. Book, reader, contents, dictionary, statistics and TXT steps need fixture-specific steps: put them in the SD script.
- **Feature script.** `nav-script.txt` in the verification kit (about 19 KB, 24576-byte limit) exercises the reader (open, page turns, list and toolbar menu), the toolbar's Contents, Text and More panels (scroll, Reading stats, Go to % with adjust and confirm, chapter jump and the chapter boundary in both directions), Contents on a big EPUB (scroll, wrap, late jump, current jump, warm reopen), the grouped novel, dictionary lookups (English exact/stem/warm, CJK longest match with Shorter/Longer, CC-CEDICT, and three cold lookups in isolated dictionary copies), reading statistics (per book, the Settings overview and the three-minute checkpoint, see **Periodic statistics checkpoint**), the TXT chapter list with the background index wait (`txt.index.complete`, Unverified), Go to page and Go to %, prerender hits and misses (`pre.turn.*`; the hit/miss verdict is in the prerender rows, not in the step status), the Wi-Fi screen (open, Back, immediate reopen and Back, and the same screen through the Reader sync screen), a real offline Sync now attempt when no Wi-Fi network is saved anywhere and Reader sync is configured (`sync.now`, flag `sync`, see **Real Sync now**), a reader open right after the radio screens were left (`wifi.after.*`), a Library round trip (`lib.rt.*`: Title tab, two hold page jumps, open the first visible book, Back to Home, reopen; whether the viewport is kept shows in the `nav_rows` rows) and night mode and dark-menus toggles. The ids are semantic and match the emulator runner where the same step exists. Steps inside the toolbar panels and the index wait are `blind` (Unverified); every other step with an `expect` is checked. The physical-only steps are listed as `external` and never run: `sleep.reader.enter/wake`, `sleep.home.enter/wake`, `wifi.sleep.enter/wake` (sleep and wake with the Wi-Fi screens just left), `sync.attempt.online` (a sync that reaches a server, still owed on the device) and `sync.after.sleep`. Copy the script to `/.crosspoint/benchmark/nav-script.txt`. Fixtures it needs on the card: `/navfeat/probebook.epub`, `/navfeat/spinebook.epub`, `/navfeat/groupbook.epub`, `/navfeat/novelbook.txt`, `/.crosspoint/nav-fixtures/navigation.epub`, `/dictionaries/navigation/`, the optional cold copies `/dictionaries/navcold1/`, `navcold2/`, `navcold3/` and `/dictionaries/cedict/` (`cedict.ifo`, `.idx`, `.dict`, `LICENSE.txt`, `ATTRIBUTION.txt`); every segment is skipped, not passed, when its fixture is absent. Key counts in the script (menu rows, tab positions, the Go to % row counted back from the end of the More list) come from reading the source, not from a hardware run: a count that is wrong stops the run at the next guard or `expect` instead of pressing on. `lib.rt.open_book` opens whichever book is first in the Title view on your card and `lib.rt.home` presses Back at it; that book's reading position and statistics are put back by the restore, its cache stays.
- **Periodic statistics checkpoint (`stats.periodic.*`).** The checkpoint fires from the reader's loop once 180 s have passed since the book's statistics started or since the previous flush of any kind (a menu cover counts), so the physical script runs the timer down on purpose: open the big EPUB, three `idle:stats-timer` steps of 55 s (`norefresh` is 16 bit, so one idle step cannot span 180 s), then 40 Up turns from the last chapter, where the earlier `spine.*` steps leave the book (a Down series would hit the end of the book and stop refreshing). A wait never proves a save: the `stats_flush` row with reason Periodic and result Saved does, and `nav_device_accept.py` reports where it fell, **overlapped** (it started while a turn was in flight) or **gap between reps** (it started in the quiet time between two turns: the save is proven and sits just before the next press, overlap with a turn is not). A flush before the first or after the last turn, a flush that did not save, or a turn that did not refresh fails the check. The reading-stats menu-cover snapshot (`stats.book.open`) flushes through the suspend path and is not periodic coverage.
- **Which presses are real buttons on the X4 Pro.** The X4 Pro has two physical side buttons on the screen edges (Up on the left, Down on the right) and Power; Back, Confirm, Left and Right come from the touch panel and the capacitive Home key. The run injects at the button layer, so a `Back`, `Confirm`, `Left` or `Right` step is a virtual front key (the same press a tap or the Home key produces), and `Up`/`Down` are the real side buttons. In the toolbar the side buttons scrub chapters (`tool.scrub.right` = Down x3, `tool.scrub.left` = Up x3, same ids and keys as the emulator run; each press must refresh), while Left/Right cycle the tool focus and are recorded as `tool.focus.virtual.*` so a virtual press is never compared with a real one under one id. The Go to % dialog has both: Left/Right step 1% (`tool.goto.adjust.*`, virtual) and the side buttons step 10% (`tool.goto.side.down/up`, real).
- **Repeating the script (cold and warm).** The sequencer runs the script once per start and the restore ends every run, so one run cannot repeat the script from a fresh state. Three runs in a row are **one cold and two warm** repetitions, not three cold ones: each run writes its own session file and restores reading state at its end, but the restore puts back progress and statistics files, not the cache directories the first run built, so runs two and three are warm for the book caches, the Library index and the TXT index. A fresh-cache pass needs the cache folders of the fixture books deleted by hand between runs; the run itself never deletes them. The dictionary index is the exception the script handles itself: the `dict.cold1`..`dict.cold3` segments look up the same word in three isolated copies of `/dictionaries/navigation` (`/dictionaries/navcold1`, `navcold2`, `navcold3`, each with the same three files). A copy has no index sidecar until its first lookup, so each of the three is one cold repetition in a single run, and no user cache is deleted. A copy is cold only until it has been used once (recopy the folder to repeat), and the copies are separate on-card fixtures: they are equivalent to `navigation` only when they are byte-identical copies of it, which this guide does not check.
- **Backup and restore.** Before the first step the run snapshots, on the card under `/.crosspoint/benchmark/nav-backup/`, `recent.json`, `state.json`, `settings.json`, `reader-sync/status.json`, every `reading-stats/` file and every existing `progress.bin` of a book cache. The card is walked as a stream and files are copied and checked in 1 KiB pieces, so memory use does not grow with the library (a card with 1,700 book caches is fine); the only limits are explicit ones: at most 1536 recorded files of at most 32 KiB each. A card over a limit is never partly protected or truncated: the run does not start (**Backup of reading state failed**) and the partial backup is removed. When the run ends, diverges, times out or is interrupted it goes to Settings, waits for the screen to go quiet (10 s at most), puts every snapshot file back (verified by reading it again; larger files are assembled beside the original and renamed over it), deletes the files the run created next to them (new progress and statistics files), re-reads settings, recents, state and statistics, and records a `nav_restore` row. The manifest is written last and deleted last: if the reader crashes, loses power or sleeps mid-run, the next boot restores before anything reads the state. A failed restore is logged (`BENCH`, with a hash of the first failed path), shown on the Benchmark screen as **RESTORE FAILED**, keeps the backup, makes the footer a failure, and blocks the next run (**An earlier run is not restored yet**) until a retry succeeds. Book files, reader caches and Wi-Fi credentials are not part of the snapshot; cache directories that a run builds for a fixture book stay on the card.
- **Injection.** Presses are injected at the physical-button layer of `HalGPIO`, so mapping, long-press and hold-repeat logic run unchanged. A press holds for its `hold` time and releases; presses are never queued. Any real button or touch edge ends the run at once (outcome *interrupted*, footer "cancelled"); the power button is never injected, and a real power press also sleeps the reader as usual. While a run is active, automatic sleep, the frontlight gesture and reader sync are inhibited, as in the page-turn run. Recording alone (`benchmarkRecord=1`) does not inhibit anything: reader sync is held off only while a navigation run is active (`benchmark::nav::runActive()`) or a benchmark activity is in front (the Benchmark screen, a benchmark reader run, the benchmark image viewer); in that state `ActivityManager` reports the screen as not permitted to the reader-sync scheduler (`UiState.permitted`) and skips the healthy-render mark, so the scheduler never launches its worker. The one exception is a `sync` step (see **Real Sync now**).
- **Real Sync now (`sync`).** A step marked `sync` (one key press, no `x=`, no `do=`) is a real Reader sync attempt, not a press on a suppressed screen. It runs only when `needs=syncsafe` holds: Reader sync is configured (`/.crosspoint/reader-sync.json` or its `.bak`), there is no `/.crosspoint/wifi.json`, the in-memory credential store is empty, and the Wi-Fi interface is off and not connected. Otherwise the step is skipped, and the physical feature is still owed. When the press arrives the run re-checks all of that (a failed re-check diverges the run, nothing is permitted) and then permits the real scheduler for this step only: the Reader sync screen is allowed to launch the worker, everywhere else the inhibition above stays. The step does not settle until the worker reaches a terminal phase (counted by `finishedRuns`) and the screen redraws; the phase must be **Offline** (no saved network, nothing reached) or the run diverges, and a network path appearing while the worker runs cancels it and diverges the run. No credential exists, so no network traffic is possible; an online attempt remains `external` (`sync.attempt.online`). The real worker also writes its `SyncRun` event (trigger, phase, duration; zero bytes) into the session, and the attempt persists `/.crosspoint/reader-sync/status.json`, which the run backs up and restores. In-memory scheduler state (last attempt time, next due time) is not rewound; it only affects the next automatic attempt.
- **Timing.** Per measured step: `start_us` is the injection time; `first_submit_us` is when the first display call of the step **began** (submission, not completion) and is only counted if it started after the press; `end_us` is the end of the last display/BUSY call of the step, a software wait-call interval, **not optical settling**. Refreshes are attributed by time window alone: a clock tick or background render inside the window is counted too. SD time inside a step (indexing, cache reads) is not separated from the rest.
- **Result.** A run writes its own `/Benchmark/logs/session-….jsonl`, retained in the menu row (**Run navigation benchmark**) as status, step counts (ok / unverified / no refresh / n/a / external / failed), the path and **Reading state restored**. **Finished; result saved** appears only for outcome *complete* with zero failures and a successful restore; it does not turn Unverified steps into passes, the Unverified count stays beside it. When the run ends it returns to the Benchmark menu.

**Navigation run event data**


| Event | `data` slots, in order |
| --- | --- |
| `nav_step` | 1-based step index (every step line counts), repetition (0-based), step-id FNV-1a hash, microseconds from injection to the first display-call start (0 when none), first display call \| first mode<<8 \| last call<<16 \| last mode<<24 (call: 1 buffer / 2 refresh / 3 async wait / 4 gray base / 5 gray / 6 configured gray base; async submits are reported as the refresh they complete), refresh count (low 16 bits) \| status<<16, internal heap minimum-ever free, lowest internal free bytes sampled while the step ran. `start_us` is the injection, `end_us` the settled instant. Status: 0 ok / 1 no refresh / 2 timeout / 3 unsupported / 4 diverged / 5 skipped / 6 external / 7 unverified (a `blind` step that settled; timings are real, the screen is not proven). |
| `nav_result` | Outcome: 0 complete / 1 cancelled / 2 timeout / 3 diverged / 4 bad script / 5 recorder / 6 unmappable key / 7 interrupted; step lines visited; measured rows; ok \| no-refresh<<16; skipped \| external<<16 (bad script: error line); failed \| unverified<<16 (bad script: error code; slot 5 used to hold `failed` alone, so mask the low 16 bits); elapsed ms; FNV-1a of the script text. |
| `nav_input` | Any press, any release, touch activity; edge injected by the run (1) or real (0); current activity-name hash. Written only while a navigation run is active, for every activity, with no key identity. |
| `nav_rows` | Library/contents row-cache event emitted by the list screens: screen (1 library / 2 contents), 1 cache hit / 0 load, rows loaded. |
| `nav_restore` | Restore ok (1/0), entries attempted, files restored, files removed (absent at snapshot), extra files removed, failed, hash of the first failed path, manifest found (1/0). One row per run, written after the restore and before the footer. |
| `stats_flush` | One `reading_stats` flush that reached an open book: reason (0 periodic checkpoint / 1 cover or suspend / 2 leaving the reader / 3 direct call), result (0 nothing dirty / 1 saved / 2 failed), book and summary slot generation after the call, ms since the previous flush call, ms past the 180000 ms due time (periodic only), 1 when totals were dirty on entry, 0. The span is the call: directory check, two checksummed commits (write, sync, rename). Appended after the reserved retired probe slot; existing event names and data meanings are unchanged. |

For source builds, use `pio run -e x4pro-gh_release` (INFO release profile; no wait for a serial host). Host recorder, page-clearing policy and prerender-key regressions are part of the existing CMake suite: `cmake -S test -B /tmp/reader-benchmark-tests`, `cmake --build /tmp/reader-benchmark-tests --target BenchmarkTest LibraryBuilderTest AdaptiveRefreshPolicyTest RefreshCalibrationLadderTest EpubPrerenderKeyTest`, then `ctest --test-dir /tmp/reader-benchmark-tests -R 'Benchmark|BatterySampling|RefreshClearRows|LibraryBuilder|AdaptiveRefresh|RefreshCalibration|EpubPrerenderKey' --output-on-failure`. These checks do not replace emulator or physical-device validation.

The navigation run's script parser, sequencer, probe, built-in script and `nav_*` encoders are covered by `NavScriptTest`, the feature table by `NavFeaturesTest`, and the backup, restore, crash-recovery and failed-restore paths (against an in-memory card that can fail on demand) by `NavRestoreTest`, all in the same `BenchmarkTest` binary. Their names do not match the regex above, so add `Nav`: `ctest --test-dir /tmp/reader-benchmark-tests -R 'Benchmark|BatterySampling|Nav' --output-on-failure`; set `NAV_FEATURE_SCRIPT=<path to nav-script.txt>` to also parse the staged feature script against the grammar, the 24576-byte limit and the feature table. Host tests do not exercise the HalGPIO injection, the device card access, the activities or the display; the run itself needs a device (or an emulator whose HAL implements `HalGPIO::injectPress`, `injectionInFlight`, `setInjectionGuard`, `realInputSeen` and `lastEdgeInjected`).

**Measured result (emulator, current candidate).** The candidate (application SHA-256 `dcb9d3668f5308bb9e46050fc6bfb9a1e1fa3c3992654143fe977ec1ff805aa1`, ELF `356c2aa282abb6668f12b241e00e2c140a03ebf559fb0fb8c475a8c756512325`) was compared with the frozen pre-change baseline (application `07e5a427…`, ELF `b65173f2…`). Both ran the automatic benchmark with the same seeds, reader font (14 pt LXGW WenKai SD font, text anti-aliasing on) and UC8279 display-timing model, once in night mode and once in light mode. All four runs completed with 10/3/5/1 turns, a complete footer, zero queue losses and zero discarded events. Medians, baseline → candidate:

| Measurement | Night | Light |
| --- | --- | --- |
| EPUB later page, render call | 2268 → 509 ms | 3013 → 1269 ms |
| EPUB later page, non-display CPU | 1775 → 17 ms | 1796 → 52 ms |
| EPUB warm reopen, later page | 2277 → 510 ms | 3028 → 1269 ms |
| EPUB first page after open | 7.50 → 3.86 s | 8.15 → 4.48 s |
| TXT later page, render call | 5228 → 561 ms | 5973 → 1319 ms |
| TXT first open (index + first page) | 27.3 → 20.9 s | 28.1 → 21.6 s |
| BMP paint (FAST refreshes) | 2128 → 1140 ms (4 → 2) | 2231 → 1253 ms (4 → 2) |
| Whole run, including about 38 s of intentional dwell | 151.9 → 86.9 s | 167.4 → 103.3 s |

Display time per page is unchanged (about 493 ms FAST in night mode, and about 1217 ms in light mode, where FAST is followed by the grayscale pass). The page-turn gain is CPU work removed before the refresh. Most of it comes from the SD font cache: EPUB font prewarm fell from 1656 ms to 1 ms, and black-and-white text drawing from 97 ms to 10 ms. This is because cached glyphs now survive page turns and a cache miss costs one FreeType load. Skipping the unsupported grayscale planes in night mode saves only about 15 ms. The adaptive clearing policy does not make page turns faster. Clearing is now driven by estimated content: the baseline's fixed 15-page interval produced 3 HALF refreshes per run, all on initial pages. The candidate produced 4 HALF refreshes in night mode and 5 in light mode. On these dense CJK EPUB pages, the estimated debt cleared about every 7th page in night mode and every 5th in light mode, where anti-aliased grayscale is weighted more heavily. The five TXT turns stayed FAST after the clear that follows indexing. The 319-page TXT index stays format v3, and its page offsets are identical to the baseline's. Minimum-ever free internal RAM was 153,136 → 154,268 B. Minimum-ever free PSRAM was 5,640,252 → 5,688,628 B (night) and 5,640,236 B (light). Static RAM stayed 108,368 B, and program flash grew by 2,896 B.

Limits:

- Emulator CPU and timing are not the physical device; use these results as ratios, not latency predictions.
- Display and BUSY durations come from the modeled panel timing, not optical measurement, and no ghosting was measured.
- The shipping single-buffer display path never keeps an exact previous frame, so on hardware the policy always uses the ink-density estimate.
- Settled frames matched the baseline pixel for pixel: CJK TXT light (8 pairs), CJK TXT night (8), CJK-mixed EPUB light (15) and XTC normal/night (12). Evidence: `adaptive-perf-nxtmx4fe/report.md` and `verification-summary.json`. The candidate is staged on the SD card but has not been installed or verified on the physical reader.

### Serial diagnostics

**Serial monitor logs:** For more detailed debugging, connect the device to a computer and run the custom debugging monitor script (requires Python 3 with `pyserial`, `colorama`, and `matplotlib`; install via `pip3 install pyserial colorama matplotlib`):

```
python3 scripts/debugging_monitor.py
```

The script auto-detects the serial port. You can also specify one explicitly:

```
python3 scripts/debugging_monitor.py /dev/ttyACM0        # Linux
python3 scripts/debugging_monitor.py /dev/tty.usbmodem1  # macOS
python3 scripts/debugging_monitor.py COM7                # Windows
```

**Features:**

- Color-coded log output by category (errors, memory, display, EPUB parsing, etc.)
- Live memory usage graph (free RAM, total RAM, max contiguous allocation) updated every second
- Interactive command prompt — type a command and press Enter to send it to the device
- Screenshot capture — saves the current display to `screenshot.bmp` when triggered by the device

**Options:**

| Option               | Description                                               |
| -------------------- | --------------------------------------------------------- |
| `--baud RATE`        | Baud rate (default: 115200)                               |
| `--filter KEYWORD`   | Show only lines containing the keyword (case-insensitive) |
| `--suppress KEYWORD` | Hide lines containing the keyword (case-insensitive)      |

**Examples:**

```
# Show only memory-related log lines
python3 scripts/debugging_monitor.py --filter MEM

# Hide noisy SD card log lines
python3 scripts/debugging_monitor.py --suppress "[SD]"
```

Press **Ctrl-C** or close the graph window to exit.

If the device is stuck in a bootloop, press and release the Reset button. Then, press and hold on to the configured Back button and the Power Button to boot to the Home Screen.

There can be issues with broken cache or config. In this case, delete the `.crosspoint` directory on your SD card (or consider deleting only `settings.json`, `state.json`, or `epub_*` cache directories in the `.crosspoint/` folder).

## 8. Local reading inbox

Open **Home → Reading inbox** to return to saved annotations across all books. On the cover-grid Home layout, use the **book icon between Library and file transfer** (before OPDS when configured). This is an optional local workflow, not a network service: opening the inbox never connects to robertzhou.dev or enables cloud sync.

- Save a **Quote**, **Question**, or **Review later** annotation in an EPUB-based reader to add an item. The inbox reads the existing native annotations; it does not copy them into another library or invent a separate source identity.
- Rows show the source title, original filename and chapter, annotation kind, and progress at capture. Select a row to open its original document at the saved chapter/XPath and visible-text offset, rather than resuming the book's most recent page. Converted Markdown, FB2, and HTML use their original paths, not a temporary EPUB filename. Return accuracy assumes the source content has not been changed; pagination can change with font settings.
- There are still at most **128 annotations per book**. The inbox retains only **32 lightweight previews at a time** (about 10 KiB), with **Previous 32 items** / **Next 32 items** for additional pages. Items follow the SD annotation-directory order and native IDs, not an assumed chronological order. **Refresh inbox** rescans from the first page; entering or explicitly changing pages also scans the store. Ordinary row navigation and rendering do not scan files or parse JSON.
- An empty inbox says there are no saved items. A missing source stays listed, but opening it reports the missing original path; restoring the file at that path allows return again. A corrupt/unreadable native record reports a load error instead of presenting a silently incomplete inbox.

### Exporting the whole inbox for the Mac companion

Choose **Export all inbox entries** to write `/annotations/reading-inbox.md` on the SD card. This exports **every native annotation across all books**, not just the visible 32-item page. An empty store produces an honest empty export. Copy the file using the existing USB/file-transfer workflow and pass it to the Mac companion's annotation importer. Exporting does not send anything automatically.

The Markdown uses the same per-entry interchange fields as book-local annotation exports: original **Source** path, **Title**, **Type**, chapter title/path, spine index, XPath, optional visible-text offset, capture progress/page context, excerpt, note, and a UTC creation timestamp when the device clock was set. The document header says **Scope: All sources**; each entry carries its own source. Numeric annotation IDs are book-local, so companion return/sync identity is the **original source path plus entry ID**, not the heading number alone. These original paths and location fields are the local annotation-return handoff for a separately connected, opt-in robertzhou.dev transport.

Exports are streamed one record at a time. A failed read/write does not replace the last complete export; success is shown only after the replacement is committed. This is a local snapshot, not a cloud queue, remote bookmark downloader, or two-way cloud-sync implementation. Keep `/.crosspoint/annotations/` and the original documents backed up: clearing all of `/.crosspoint/` also deletes the native inbox entries.

## 9. Private Wi-Fi Reader sync

**Verification status:** the installed image is SHA256 `2383850eeb9d33447c418ba9ea524f4750aae832a5319754efbc4725bc3deac6` (ELF `692b021665f30b12b9ff4914f9c38e65446db431049d4cf93466df486b47b0db`). It adds the incremental TXT reader (first page before the full index, chapters and TXT menu, UTF-8-safe windows, CJK line breaking and paragraph style), the idle EPUB next-page prerender, event-driven Reader sync, input latching during background work and the UI fixes. It built for `x4pro-gh_release` (flash 95.1%), and 608 native CTest tests, the reader-sync Python suites, 226,398 FreeInkUI host checks and 244 companion tests passed. Emulator acceptance (`next-txt-ui-sync/report.md`, 58 checks) passed: both OTA slots reach Home, deferred acceptance happens exactly once, interrupted or SD-less boots stay PENDING_VERIFY, night/light benchmarks show no EPUB regression, Latin TXT pagination is unchanged, CJK EPUB/XTC pages are pixel-identical, TXT progress migrates from the previous firmware, and real-novel navigation, touch and 120 ms button presses work while indexing.

On the physical reader it completed the automatic benchmark (session `138c9526`, same settings as the previous image's session `b03ea87c`) with no lost events: first TXT page 31.65 s → 2.27 s, TXT page render median 720 → 644 ms, EPUB page render median 652 → 622 ms, with every forward EPUB turn served from the prerendered page (`adaptive-perf-nxtmx4fe/physical-device-20261101/comparison-vs-b03ea87c.json`). These are software render spans, not button-to-pixel latency. The previous image `dcb9d3668f5308bb9e46050fc6bfb9a1e1fa3c3992654143fe977ec1ff805aa1` remains the reviewed rollback image. No check so far establishes physical Wi-Fi/cloud end-to-end operation, battery consumption, optical latency or ghosting, the light-mode physical benchmark, or full cold-power-cut rollback. The known emulator cold-start flash-erase limitation remains.

**Staged, not yet installed (rc8):** the card's `reader-workbench-rollback-reviewed.bin` is now SHA256 `9c691695e2aeefa928d9528b40a3ce97feb46dbabacd9ce3b5306659fd43330d` (ELF `66d4bbe9e6354da10114980b8f824c6520199d3da87f739ea6f195af27bc2398`, 6,450,496 bytes). It adds the Wi-Fi sync TLS fix (SHA-384/P-384 chains), Colby Access (PEAP/MSCHAPv2 with `/.crosspoint/wifi-ca/colby.pem`), Wi-Fi failure reasons and `/.crosspoint/wifi-log.txt`, night-mode flash fixes, big-EPUB tables, background library indexing, the CC-CEDICT dictionary (`/dictionaries/cedict/`) and reading statistics. It passed 644 host tests and emulator acceptance (`next-txt-ui-sync/rc8-report.md`); it has not yet run on the physical reader. After installing, the image above stays in the other slot as the fallback.

The separately provisioned service downloads the private robertzhou.dev ready queue directly over saved Wi-Fi to `/books/Inbox`, and uploads native annotations. It does not require routine Mac/SD delivery. A collector on a Windows WSL host gathers RSS feeds, shared links, signed-in Substack subscriptions and the X Following feed for the site every 15 minutes. It runs on a persistent `OnCalendar` timer, so a run missed while the host was off happens on the next start. Its dedicated Chrome profile is signed in by hand; to sign in again from the Mac, run `ssh -L 6080:127.0.0.1:6080 windows "wsl -d Ubuntu -- /home/bob/reader-collector/bin/login-profile-remote"` and open `http://localhost:6080/vnc.html`. The Mac LaunchAgent `dev.reader-workbench.reader-send-subscriptions` is disabled (`launchctl enable gui/$(id -u)/dev.reader-workbench.reader-send-subscriptions` and `launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/dev.reader-workbench.reader-send-subscriptions.plist` restore it). The site holds at most 256 unread books (128 MiB); when the reader has not synced for a while, uploads stop with HTTP 507 and resume after the reader drains the queue. Powering off the reader does not make that material available on its SD card.

Open **Settings → System → Reader sync**:

- **Enabled** changes only the enable flag, preserving the private device key.
- **Schedule** switches between **Wake, sleep, charging** (event-driven; the default) and **Fixed interval** (the previous periodic behaviour). The choice is saved in `reader-sync.json` as `schedule`; see **Schedule** below. It is an on-device setting only: like the rest of Reader sync it is not exposed through the web settings API.
- **Sync now** requests a background attempt; it does not freeze the reading loop. Server rate-limit cooldowns still apply.
- **Choose Wi-Fi** uses the existing saved-network selector. Selecting it first cancels and joins any background transfer.
- **Status**, **Last success** (UTC), **Books received**, and **Notes sent / skipped** show nonsecret state. Offline, invalid configuration, clock, authentication, SD-space, memory and rejected-note conditions remain visible.

The SD provisioner creates `/.crosspoint/reader-sync.json` with six fields: `version: 1`, HTTPS `origin`, scoped `device_key` (64 lowercase hex characters), stable `device_id` (SD volume UUID), boolean `enabled`, and `interval_seconds` (60–86400, normally 900). A seventh, optional field `schedule` is `"events"` or `"interval"`. **Backward compatibility:** a six-field file, including every existing one with `interval_seconds: 900`, still loads unchanged and runs the event-driven schedule. The firmware writes `schedule` only after you choose one in Settings, so an untouched card stays readable by older firmware. Once `schedule` is present, firmware from before this change rejects the file (it requires exactly six fields) and disables sync: roll back by deleting that key. Any other `schedule` value, or any other extra field, is invalid. `card provision` writes six fields on a fresh card and preserves an existing `schedule`. Never use the management or share key. Missing or invalid configuration safely disables the service. The key is physically readable on the SD card, not encrypted; it is restricted and revocable, is never displayed in settings/status, and is blocked from generic web/WebDAV file APIs. Do not publish the file or include it in support logs.

**Schedule.** One lazy worker starts only after a completed healthy Home/reader render and never during a benchmark workload. It does not wake a sleeping reader. With the default `"schedule": "events"`, only these events start a sync:

- **Wake or boot:** the first time Home is on screen after the reader boots or wakes. A sync therefore never competes with a book that is still opening, and waking straight into a book defers the wake sync until Home is next shown. It is skipped, and not retried later in that session, when the last successful sync is under 15 minutes old or any attempt, even a failed one, is under 5 minutes old; an offline reader does not scan for Wi-Fi on every wake.
- **Before deep sleep:** only if an annotation was saved since the last clean upload pass. A private marker, `/.crosspoint/reader-sync/notes.pending`, records this and survives a reboot or power loss. The sleep screen is already drawn when the sync starts. It is the normal sync run (it may also fetch waiting books), but it is given 20 seconds and then told to stop, so sleep is delayed by at most that plus the time an in-flight request needs to unwind (bounded by the HTTP timeout). It is skipped when an attempt ran in the last 5 minutes, in Fixed-interval mode, and after benchmark runs. A stopped attempt is recorded as an attempt, so the next sleep does not wait again at once.
- **While charging:** when USB power is present, once every `interval_seconds` (900 s by default), counted from the last run. That age is persisted, so it survives reboots. On battery nothing runs periodically.
- **More work pending:** after a bounded batch reports more books or notes, the next batch follows at least 60 seconds later, charging or not.
- **Manual:** **Sync now**, or switching **Enabled** on. It bypasses the debounce and the book-opening rule but not a server cool-down.

`"schedule": "interval"` keeps the previous behaviour exactly: one attempt after the first healthy render following boot or wake, then every `interval_seconds` of awake time, whatever the power or screen; no wake debounce and no pre-sleep sync. `interval_seconds` keeps its meaning in both modes. Bounded batches drain at least 60 seconds apart; 429 respects `Retry-After` (and also holds back **Sync now**), and 503 parks until the next wake. A cancelled run is retried after 60 seconds. Page turns continue on the main loop. Navigation into another activity, foreground networking, USB handoff and sleep cancel/join the worker before sharing the radio or SD.

**Radio.** Every attempt ends with Wi-Fi fully off (`WIFI_OFF`; saved credentials are kept) before it is reported finished. If the first switch-off does not take, the station is disconnected and switched off again; a persistent failure is logged as `radio still on after sync` and flagged in the sync log. An attempt only switches off a radio it actually started; normal background completion does not reboot the reader.

**Sync log.** Each run is logged on the serial console (INFO) as `RSYNC run trigger=… phase=… ms=… rx=… tx=… books=… notes=… mv=<before>><after> pct=<before>><after> radio_off=1`. `trigger` is `manual`, `wake`, `pre_sleep`, `charging`, `interval` or `continue`. `ms` runs from worker start to radio off. `rx`/`tx` are HTTP body bytes received and sent (headers, TLS framing and DNS excluded). Battery is read just before the radio starts and again once it is off; the second reading is taken by the main loop, so it can lag the radio-off moment by a loop pass. With **Record normal use** on, the same run is also a `sync_run` row in the session log (see **Benchmark and normal-use recording**), which is how to compare wake, pre-sleep and charging syncs by battery cost.

CPU full-speed protection is reference-counted across overlapping render and sync work; one owner finishing cannot release another owner's protection. A sync attempt switches off only the radio it actually started, so missing saved credentials do not trigger a disconnect against an uninitialized station. The post-fix host ownership checks and targeted offline emulator run passed; earlier captures under `qemu-targeted-y2f78xc_` remain pre-fix evidence rather than proof of this correction.

Each EPUB is at most 8 MiB. Streaming does not require `Content-Length`: metadata size, the `X-Reader-Sha256` header, the streamed hash, and closed-file readback must all agree. Private `.part` files are never library books. The client never overwrites an existing book: it tries the source basename, `<stem>-<sha8>.epub`, then `<item-uuid>.epub`; identical verified bytes are reused. A pending private receipt, durable library-dirty marker, then completed receipt precede ACK. An interrupted install is reconciled from verified final bytes. ACK releases the server's content bytes, so a later fresh download requires re-delivery under a new item ID.

Native annotation files are not rewritten to insert cloud IDs. Upload IDs and anchors use the companion's exact sorted compact UTF-8 JSON/UUID5 mapping, with the SD UUID as `device_id` and `item_id: null`; Mac fallback and device uploads therefore share an identity. Accepted UUID/digest receipts live under `/.crosspoint/reader-sync/`, never the annotation tree. Oversized/invalid notes stay local without truncation; unchanged deterministic server rejections are recorded separately and are not retried in a tight loop. The annotation service is append-only: deleting a local note does not delete its cloud copy.

Annotation batches inspect at most 256 native records and make at most eight upload attempts. A private durable scan cursor advances across the entire native store, including stores larger than one batch; a vanished cursor restarts safely using accepted UUID receipts.

Release-integrator checks, from the repository root:

```sh
/Users/b/.platformio/penv/bin/pio run -e x4pro-gh_release
python3 test/reader_sync/run_protocol.py
python3 test/reader_sync/canonical_oracle.py
python3 test/reader_sync/power_lifecycle.py
cmake -S test -B /tmp/reader-sync-tests
cmake --build /tmp/reader-sync-tests --target ReaderSyncScheduleTest BenchmarkTest
ctest --test-dir /tmp/reader-sync-tests -R 'ReaderSyncSchedule|BatterySampling|Benchmark' --output-on-failure
```

The protocol runner builds the real `SecureHttpClient`/`SecureClient` and pinned wolfSSL against an isolated HTTPS peer, with POSIX storage/TCP adapters and temporary credentials. It exercises chunked/close-delimited payloads, size/hash failures, cancellation, full/write-failed storage, rename failure, collision refusal/reuse, interrupted installs, pagination/cursor recovery, canonical annotation receipts, bounded annotation traversal and rate/auth errors. It also checks configuration compatibility (a six-field file with `interval_seconds: 900` loads as the default schedule and is written back with six fields, explicit `schedule` values round-trip, invalid or extra fields are rejected) and that received/sent byte counts match the traffic. `ReaderSyncScheduleTest` covers the pure trigger decision in `ReaderSyncSchedule.h` (state in, sync or no sync out): wake debounce, Home and book-opening gating, charging pacing, pre-sleep conditions, back-off and the Fixed-interval mode. It never accesses production or a real card. Run the canonical cross-language fixture runner and the existing deferred-OTA/QEMU healthy-render/offline cases against the final image. QEMU cannot prove the physical radio, simultaneous panel latency or battery cost.

The power-lifecycle runner compiles the production power HAL with host clock/Wi-Fi/semaphore adapters. It checks overlapping owners in both release orders, real cross-thread overlap, hardware clock-write failure retries, and the independent Wi-Fi full-speed override; it does not measure physical clock or battery behavior.

### SD-card fallback: delivery by plugging the card into the Mac

When the reader cannot reach Wi-Fi (or Wi-Fi sync is not installed yet), plug the SD card into the Mac and wait for the notification "N books delivered to the reader card". A user LaunchAgent (`dev.reader-workbench.card-autosync`, started by macOS whenever any volume mounts) runs `reader-send card autosync`, which acts only for the configured card's exact volume UUID: it downloads the ready books from the private queue, copies them into `/books/Inbox` on the card, reads every copy back and checks its SHA-256, and only then acknowledges those books on the server. A book whose copy does not verify is left on the server and retried at the next plug-in; HTTP 429 (rate limit) stops the run and the rest follows next time. It also copies the card's annotations to `~/papyrix/annotations`. It never ejects the card, never touches `/books/Novels` or `/books/Books`, never deletes a book and never installs firmware. Eject the card in Finder yourself, put it back in the reader, and the new books appear in Browse Files/Library (the library rescans because the delivery sets the `library.dirty` marker). The run log is `~/Library/Logs/reader-card-autosync.log`; the management key is read from the Mac Keychain at run time and is never written to the card, the log or the agent file. Setup and details: `companion/README.md`, "Automatic delivery when the card is plugged in".
