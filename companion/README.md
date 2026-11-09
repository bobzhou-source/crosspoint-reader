# Reader Workbench companion

`reader-send` turns public Substack/generic web articles or locally saved HTML into EPUB, then optionally sends the EPUB to the reader with LocalSend v2. Standard public-article conversion and LocalSend run on your computer and require no cloud account, firmware web fetching, browser automation, cookies, or passwords. A separate, explicitly selected `cloud` command can sync already-converted EPUBs and annotations to a private reader API; the user-authorized scheduled `collect` pipeline also uses that API automatically, but manual fetch/convert/send and subscription collection alone do not.

## Install

Python **3.11+**:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install ./companion
reader-send --help
```

On Windows, activate with `.venv\Scripts\Activate.ps1`. If already inside `companion/`, install with `python -m pip install .`. Pillow is the only runtime dependency for standard article conversion and the card/cloud commands; parsing, HTTP, caching, EPUB and LocalSend use the standard library. `reader-send x --browser`, `x-following`, `subscriptions` sources added with `--browser-url`, and a pre-existing **Send X thread.command** additionally need the optional browser extra: `python -m pip install './companion[browser]'` (or `.[browser]` inside `companion/`), which installs `websockets` for Chrome DevTools capture. HTTPS still needs a CA trust store: on a python.org macOS installation run its **Install Certificates.command**, or `python -m pip install certifi` and set `SSL_CERT_FILE=$(python -m certifi)` before using `cloud` or `assist`. Never disable certificate or hostname validation.

## Chinese-English offline dictionary

From the repository root, `python3 companion/scripts/build_cedict.py OUTPUT/cedict` downloads the official [MDBG CC-CEDICT release](https://www.mdbg.net/chinese/dictionary?page=cc-cedict) and the [CC BY-SA 4.0 license](https://creativecommons.org/licenses/by-sa/4.0/), then creates uncompressed `cedict.ifo`, `cedict.idx` and `cedict.dict`. Copy the entire `cedict` folder under the reader's `/dictionaries/` and select it in **Settings → Reader → Dictionary**. The generator never writes to a card itself.

Definitions contain tone-mark pinyin and English glosses. Simplified and traditional forms share all their senses; ASCII-case-equivalent headwords are merged. Index ordering folds only ASCII bytes, matching firmware `StringUtils::asciiCaseCmp` with UTF-8 bytes in the C locale (not Python Unicode `casefold`). The reader matches up to eight CJK characters from the selected start, bounded by punctuation and the visible line.

`LICENSE.txt`, `ATTRIBUTION.txt`, the original `cedict-source.txt.gz`, `manifest.json` and `SHA256SUMS` accompany the output. Keep the license and attribution alongside redistributed files. Conversion changes are identified in attribution; the converted data remains CC BY-SA 4.0. To reproduce a pinned release offline, use `--source PATH/cedict-source.txt.gz --license PATH/LICENSE.txt`. If a download is blocked, download those two official files manually and pass those options; never disable HTTPS certificate checks. On python.org macOS, configure its certificate store (or `SSL_CERT_FILE=/etc/ssl/cert.pem`) as described above.

Behavioral tests: `python3 -m pytest companion/tests/test_cedict.py`. Production C++ longest-prefix/UTF-8-ordering tests are registered under `test/cjk_dictionary`; its executable optionally accepts an extensionless generated dictionary path to validate every index entry with the actual firmware comparator.

## English offline dictionary

`python3 companion/scripts/build_english_dict.py OUTPUT/english` downloads [Open English WordNet 2025](https://en-word.net/) (CC BY 4.0, WNDB format), WordNet 3.0 (its irregular-inflection `.exc` lists only) and the CMU Pronouncing Dictionary (only to attest regular inflections), then writes `english.ifo`, `english.idx`, `english.dict` and `english.syn` (`sametypesequence=m`). Pass `--oewn`, `--wordnet30` and `--cmudict` with local copies of those archives to build offline. `scripts/english_function_words.txt` supplies articles, pronouns, prepositions, conjunctions, modals and contractions, which WordNet omits.

Each definition is plain text, one line per item: a `(noun)`/`(verb)`/… label, then numbered senses `1. gloss: "example" [= synonyms]`, at most ten per part of speech, ordered by SemCor frequency. The definition screen collapses blank lines and strips leading spaces, so the format uses neither. Irregular forms from the WordNet exception lists go to `.syn` (`went → go`, `children → child`, `is → be`), as do regular forms the firmware stemmer cannot undo (`studied → study`, `happier → happy`). A form that is also a headword (`saw`, `left`, `better`) or that belongs to several lemmas (`does` → do, doe) opens with `Form of: …` and carries each lemma's entry, because an exact `.idx` hit always wins over `.syn`.

`python3 companion/scripts/verify_stardict.py FOLDER [WORDS…] [--expect WORD=LEMMA …]` mirrors the firmware lookup (`cleanWord`, sidecar bisection, `.syn`, stem variants) and checks order, spans, `.ifo` counts, every headword and every synonym. It works for `cedict` too. Tests: `python3 -m pytest companion/tests/test_english_dict.py`.

`card sync` (and `card autosync`) mirror every StarDict folder in `~/papyrix/dictionaries/` (override: `card sync --dictionaries DIR`) to `/dictionaries/<name>/` on the card, copying only missing or changed files (size and modification time) and checking each copy by SHA-256 read-back. A changed `.idx`/`.syn` drops its `.qidx`/`.sidx` sidecar so the reader rebuilds it. A folder the card already keeps under `/.dictionaries/` is updated there. Card files the Mac folder lacks are never removed.

`card sync` (and `card autosync`) also keep the reader's ConceptEngine and CS-PW kernel current from `~/papyrix/models/` (override: `--models DIR`): `platonic-hnet.bin`, `engram.bin`, `inbox-concepts.idx`, `cspw.bin` and `story-graph.idx` (the result's `models.missing` names any the folder lacks). First they refresh `inbox-concepts.idx` in that folder: an `ICPT` file (16-byte header, then one 192-byte record per EPUB in the card's `/books/Inbox` and `/books/Books`: FNV-1a 64 hash of the card path, mtime, title, and a 128-d int8 vector of the title plus the first spine text, 256 bytes in all). Vectors come from the bit-exact integer forward of `platonic-hnet.bin` (plus `engram.bin`'s unigram and exact-match bigram span lookups when present), or from the model-free byte-trigram encoder when there is no model; a book is re-encoded only when its size, mtime or the model changes, and records another exporter wrote into the index are kept. Next they refresh `story-graph.idx`, the spoiler-gated `SGRF` story index (`reader_send/story_graph.py` documents the 512-byte-sector layout): a prebuilt one (the teacher exporter's, e.g. written with `story_graph.load_teacher_timelines` + `build_story_graph_index`) is kept as is; otherwise one is built from the card's `/books/Books`, `/books/Inbox` and `/books/Novels` EPUBs, with characters, places and concepts found heuristically, one event per chapter that mentions them (gloss, co-occurring entity, 128-d vector), each gated at its reading position `((spine + 1) << 16) | (codepoint >> 6)`. Unchanged books keep their records; new ones are extracted (up to about a minute a book with the pure-Python model) and no new book starts after 60 seconds, so the rest wait for the next sync. Then every `.bin`/`.idx` in the folder is copied to `/models/` when missing or changed (size and modification time), each copy checked by SHA-256 read-back. Each sync also backs up the card's `/Benchmark/logs/session-*.jsonl` and `/.crosspoint/liquid-refresh.bin` into `~/papyrix/verification/refresh-calib/usage/` (override: `--usage DIR`).

## Fetch, convert, inspect, send

```sh
# Public HTTP article: create an EPUB in the local cache, without sending.
reader-send fetch 'https://publication.substack.com/p/post-slug'
reader-send fetch 'https://example.com/article' --output ./articles/

# An article saved from your own authenticated browser session.
# File > Save Page As > HTML (or complete webpage), not a PDF/WebArchive/MHTML.
reader-send convert ./saved-post.html --output ./saved-post.epub
reader-send convert ./saved-post.html --source-url 'https://publication.substack.com/p/post-slug'

# Substack writer export: ZIP or an unzipped directory of HTML posts.
# posts.csv post_id/title/post_date/post_url metadata is used when present.
# Produces one EPUB per HTML post; the ZIP is never extracted to disk.
reader-send convert ./substack-export.zip --output ./articles/
reader-send convert ./substack-export/ --output ./articles/

reader-send inspect
reader-send inspect FULL_24_CHARACTER_CACHE_ID
reader-send inspect --json

# Enable LocalSend on the reader and join the same LAN first.
reader-send discover
reader-send send ./saved-post.epub                         # discover one HTTP receiver
reader-send send FULL_24_CHARACTER_CACHE_ID --reader 192.168.1.42:53317
reader-send fetch 'https://example.com/article' --send --reader 192.168.1.42:53317

# After the one-time card setup below, deliver every verified cached article.
reader-send card sync

# Manual fallback: copy an exported EPUB to the reader's inbox.
reader-send convert ./saved-post.html --output /Volumes/READER/books/Inbox/
```

`--title`, `--author`, and `--date` override extracted metadata. `--output` is an EPUB filename or directory; directory/ZIP inputs need a directory. Explicit output files are not overwritten without `--force`. Names are ASCII slugs plus deterministic hash suffixes, at most 90 bytes. `--reader` requires `--send` on fetch/convert, so a target flag alone cannot silently initiate delivery. Multiple discovered receivers require explicit selection.

### Authenticated posts and extraction

Only normal public HTTP GET requests are made. `robots.txt` is checked per origin, including article/image redirects; disallowed or unavailable robots policies stop retrieval. Publisher terms and your authorization still apply. There is no paywall bypass, credential scraping, or JavaScript renderer. A public preview remains a preview; detectable paywalls generate a warning. For subscriber posts, save the **full rendered article you are authorized to access** or supply your own export. Export HTML must actually contain the text; login/app shells cannot be reconstructed.

Metadata comes from Open Graph, HTML headings/bylines/time tags and Article JSON-LD. Extraction prioritizes Substack's article containers and semantic article/main elements, then scores visible prose against link-heavy containers. Scripts, forms, tracking UI, menus, ads, subscription prompts, comments and related-post widgets are removed; headings, lists, tables, quotes, links and image captions are retained. Site-specific layouts can still need local cleanup; inspect the resulting EPUB before delivery.

Local conversion is **offline by default**. Relative images must stay inside the HTML's containing folder, the supplied directory, or the ZIP; embedded image data is supported. Absolute local paths and symlink escapes are rejected. Remote image URLs in saved HTML are omitted with alt text unless `--remote-images` is supplied. With remote article fetching, images are fetched normally; `--no-images` keeps just text/alt text. Image failures warn without losing the prose.

Limits: 8 MiB per HTML/image/feed download, 100,000 HTML nodes / 128 nesting levels, 32 article images, 20 megapixels per decoded image. Images are normalized to metadata-free JPEGs, no longer than 1,200 pixels on either edge and no larger than 512 KiB each; animation uses the first frame. Export ZIPs are limited to 10,000 entries / 256 MiB uncompressed. Directory imports are limited to 1,000 HTML posts. EPUB 2 includes a proper first/uncompressed mimetype, OPF manifest, NCX and well-formed XHTML.

### Cache, dry runs and network behavior

The cache defaults to `$XDG_CACHE_HOME/reader-send`, or `~/.cache/reader-send`. Use `--cache-dir PATH` globally or after fetch/convert/inspect/send. It stores cleaned EPUBs, metadata and integrity hashes, **not** fetched raw HTML, login cookies or passwords. Locally supplied authenticated text is still private content: protect your cache and exported EPUBs as you would the original files. Remove the cache directory yourself when no longer needed.

A repeat `fetch` with the same URL/metadata/image options reuses the EPUB without HTTP. `--refresh` retrieves it again. Local conversion always rereads HTML/images so edited local assets are reflected. `inspect ID` and send-by-ID verify cached EPUB hashes; corrupt entries report how to regenerate them.

```sh
# fetch dry-run may GET the public page but does not fetch its images or write/send.
reader-send fetch 'https://example.com/article' --dry-run
# convert dry-run only reads input/metadata; no images, outputs, cache writes or delivery.
reader-send convert ./saved-post.html --dry-run
# send dry-run reads/checks/hashes the EPUB, but does no discovery or network I/O.
reader-send send ./saved-post.epub --reader 192.168.1.42:53317 --dry-run
```

Public requests identify as `reader-send/0.1`, use a 30-second socket timeout and retry transient GET failures twice with bounded backoff. `--timeout` and `--retries` adjust these; oversized and excessively slow downloads fail. Non-idempotent LocalSend prepare/upload calls are **never automatically retried**: after ambiguous failure, inspect the reader before retrying. A known session is cancelled best-effort on failure/interruption. Batch conversion is incremental; earlier successful cache/output files remain if a later post fails.

### LocalSend details and troubleshooting

The sender implements [LocalSend v2](https://github.com/localsend/protocol): IPv4 multicast `224.0.0.167:53317`, a short-lived HTTP register endpoint for discovery responses, and UDP fallback responses. `--interface YOUR_LAN_IPV4` selects a LAN interface. Discovery defaults to three seconds (`--discovery-timeout`). VPNs, AP client isolation and firewalls may block it; explicit `--reader HOST:PORT` bypasses discovery. Explicit bracketed IPv6 endpoints are accepted; multicast discovery is IPv4 only.

The firmware receiver uses **HTTP port 53317**, not TLS. This companion targets that receiver, not HTTPS/self-signed LocalSend applications. Use only a trusted LAN: HTTP provides no encryption or peer authentication, and discovery information is unauthenticated. No credentials are included in transfer requests.

Each transfer checks `/api/localsend/v2/info`, POSTs `prepare-upload` with file ID/name/byte size/MIME/SHA-256, then POSTs the binary EPUB to `upload?sessionId=...&fileId=...&token=...` with an explicit `Content-Length`. Upload is streamed from disk. The firmware saves collision-renamed files under `/received` rather than overwriting existing books. The sender honors rejected files, busy sessions, checksum failures and HTTP 204 (no upload required), and caps each EPUB at 256 MiB. A successful HTTP response is protocol acknowledgement, not a physical-display verification.

## Automatic chosen-publication inbox

Choose publications explicitly; there is no account-wide scraping. For a Substack publication the public feed is usually `https://PUBLICATION.substack.com/feed` (custom domains may also expose `/feed`).

```sh
reader-send subscriptions add essays --feed 'https://publication.substack.com/feed'
# Optional paid/full-text source: HTML saved using your existing authorized Mac session,
# or an unzipped Substack export. Credentials remain in your browser, never this tool.
reader-send subscriptions add essays --feed 'https://publication.substack.com/feed' --export-dir "$HOME/Documents/saved-essays"
# Private/browser-only publication: read its rendered archive through the Chrome session
# that is already signed in. No cookies, headers or passwords are extracted.
reader-send subscriptions add already-dawn --browser-url 'https://publication.substack.com/archive'
reader-send subscriptions list
reader-send subscriptions sync --dry-run --since-days 7
reader-send subscriptions sync --since-days 7
reader-send subscriptions sync --watch --interval 900 --since-days 7
# Recent posts from the X Following timeline through signed-in Chrome:
reader-send x-following sync --since-hours 24 --limit 50
reader-send subscriptions remove essays
```

A `--browser-url` source reads the publication's rendered archive through Chrome's loopback DevTools endpoint (`--browser-endpoint`, default `http://127.0.0.1:9333`); it requires the optional browser extra and an already-running signed-in Chrome. It never launches Chrome, extracts cookies or headers, signs in, or mutates the publication. Each pass processes sources in the fixed order local export folder, then browser archive, then RSS/Atom feed; a record with no feed does not fall back to HTTP RSS. Browser failures are reported per archive page or post, including the failing URL, and are never silently retried; `--retries` applies only to HTTP feeds/pages. Cached items use `source_type` `substack-browser` or `browser` and keep both the archive URL and final post URL in provenance. `subscriptions add` replaces the whole record, so when one publication needs several sources, pass them in one command (for example `--feed URL --export-dir DIR`). For `--watch` or the scheduled launchd job, Chrome must remain running on that endpoint; otherwise the pass records the browser error and exits 2.

`--since-days` restricts RSS/Atom and browser sources to posts published inside that window; local export folders are never date-filtered, because those are user-authorized files. Undated items are skipped with warnings and are never assumed to be recent. The scheduled collector uses a 7-day subscription window. `x-following sync` reads the already signed-in Chrome `Following` timeline, keeps a seen list and a 24-hour window in the cache, and stores EPUBs only; the scheduled collector runs it with `--limit 50`. Both collectors require the optional browser extra only when a browser source/X Following is configured. A scheduled or explicit recency pass (`subscriptions sync --since-days …`) first checks account-managed sources; a bare `subscriptions sync` without `--since-days` does not. Account-managed sources are refreshed at most once per 24 hours from the signed-in Substack settings page (`https://substack.com/settings`) through the configured DevTools Chrome endpoint. The list is accepted only when the page's “Show N more” disclosure settles to the announced total; otherwise the pass reports a `managed-refresh` error and changes nothing. Matching records get `managed_origin: substack-account` and `managed_enabled: true|false`; unsubscribed account-managed records are disabled (configuration and cached EPUBs kept, never deleted), re-listed ones are re-enabled, and genuinely new public publications are added as `https://<origin>/feed`. Private/invite-only additions use a browser source only when that settings row says so. Export-only/manual records and their cached books are never disabled or deleted by this refresh. A refresh failure is reported in that collector's errors while article sync continues from the last proven configurations.

`sync` can run once, under your own cron/launchd job, or through the explicit install hook documented below. `--watch` runs until Ctrl-C and does not install a background service. The Mac must remain running. Each pass checks the selected local/browser/RSS sources and imports up to ten new posts per publication (`--limit 1–100`). Publication feeds are read under the same **8 MiB per-download limit** as HTML and images; the DTD/ENTITY feed rejection and robots checks still apply. It fetches actual article pages rather than mistaking feed excerpts for complete posts. Feed GUIDs and canonical source IDs deduplicate imports; locally supplied full text takes priority over public previews. Changed local HTML regenerates the same source item. An empty/malformed export folder or blocked feed appears in the structured `errors` array; one-shot sync exits 2 on errors, while watch mode reports them and tries again next pass. Overlapping…

All sources share `ReadingItem` in `reader_send.model`; `reader_send.extract.Article` remains the same type for existing callers. Metadata includes `source_type`, stable `source_id`, canonical `source_url`, title/author/date, `fetched_at`, warnings, source provenance, `read_state` and fractional `progress`. Source IDs survive content updates; read/progress is stored separately from generated EPUBs and shared by cache variants:

```sh
reader-send inspect --json
reader-send inbox FULL_CACHE_ID --state reading --progress 0.35
reader-send inbox FULL_CACHE_ID --state read
reader-send inbox FULL_CACHE_ID --state unread
```

This is local inbox state, not automatic firmware reading-position synchronization. The opt-in cloud client consumes `Cache.get()` metadata and EPUB bytes without source-specific firmware; it does not change local read/progress state.

## Reader delivery, SD fallback and reviewed firmware

The card workflow is deliberately separate from LocalSend and from the optional cloud. It is available on macOS only, binds **one exact removable volume UUID**, and never guesses a disk number or `/Volumes` name.

### One-time setup

Insert the reader's SD card, then bind it explicitly. Setup verifies that the UUID is currently mounted, external/removable and writable, verifies the reviewed firmware SHA-256 before writing anything, creates the annotation output and launcher folders, stores the card configuration, and regenerates one Finder launcher:

```sh
reader-send card setup \
  --volume-uuid '00000000-0000-0000-0000-000000000000' \
  --annotations-dir "$HOME/papyrix/annotations" \
  --firmware "$HOME/papyrix/verification/reader-workbench-rollback-reviewed.bin" \
  --sha256 '2383850eeb9d33447c418ba9ea524f4750aae832a5319754efbc4725bc3deac6' \
  --launchers-dir "$HOME/papyrix/Reader Workbench"
```

The default configuration is `~/Library/Application Support/reader-send/card.json`; pass `--config PATH` to `setup`, `sync`, `status` or `update` to use another file. It contains exactly:

```json
{
  "version": 1,
  "volume_uuid": "00000000-0000-0000-0000-000000000000",
  "annotations_dir": "/Users/you/papyrix/annotations",
  "firmware_path": "/Users/you/papyrix/verification/reader-workbench-rollback-reviewed.bin",
  "firmware_sha256": "2383850eeb9d33447c418ba9ea524f4750aae832a5319754efbc4725bc3deac6",
  "launchers_dir": "/Users/you/papyrix/Reader Workbench"
}
```

Re-running setup is explicitly allowed and replaces only `Reader Workbench.command` and this configuration. Other files already in the launcher folder are left untouched. The exact UUID is required, so do not substitute a disk number or `/Volumes/NAME`.

The generated launcher is an ordinary executable `.command` file in the folder you chose:

- **Reader Workbench.command** — opens a menu:
  - **Sync now (collect)** — runs `reader-send collect --annotations-out <annotations-dir>/cloud-annotations.ndjson` (the configured annotation folder is pinned in the generated script); it fetches recent publications and the X Following timeline, drains shared links, and syncs the private site as configured.
  - **Send a link** — prompts for one URL and runs the installed **Send to Reader** Apple Shortcut through `/usr/bin/shortcuts run`. The URL is written to a temporary input file, never interpolated into a shell command. The launcher waits for the Shortcut result sheet's **Done** action before returning. If the Shortcut is not installed or no URL is entered, it reports that and sends nothing; it never adds a source URL.
  - **Update firmware (verified card update)** — runs `reader-send card update` against the configured card and reviewed image.
  - **Status** — reports `card status`, whether the **Send to Reader** Shortcut is installed, and the cache, launcher and annotation paths.
  - **Open files** — opens the launcher folder and cache, and the annotation folder when it exists.
  - **Quit**.

The menu waits for Return between actions when it has a terminal; bare `reader-send` commands do not prompt or pause. Pre-existing `Sync website.command`, `Send X thread.command` or other wrappers are not regenerated or modified by setup. There is no assist launcher and no implicit upload from bare `reader-send` commands.

### Deliver books and import annotations

```sh
reader-send card status   # read-only: is the exact configured card mounted?
reader-send card sync
```

`card status` exits zero only when the configured UUID resolves to the mounted external card; it writes nothing. `card sync` resolves the configured UUID again and fails before any write when the card is absent, unmounted, internal, not removable, read-only, or a different volume. It copies only existing cache entries whose EPUB checksum verifies, into `books/Inbox/` on the card. A destination that already contains identical bytes is counted as unchanged; different existing bytes are never overwritten: sync installs the same stem with `-<sha256[0:8]>` under `books/Inbox/`, so user books and same-named files are preserved. If a book is newly installed or an interrupted delivery is recovered from its pending ledger, the empty firmware marker `/.crosspoint/library.dirty` is created; the reader rebuilds its library on the next Library entry. Repeating sync is idempotent and reports exact copied/unchanged counts and paths.

Reader annotations are read from `/.crosspoint/annotations/` using the existing native annotation loader. They are written on the Mac as `reading-inbox-<volume-uuid>.md` under the configured annotation folder, preserving every source anchor and the native source path/chapter/XPath/location fields. When at least one real native record exists, sync also writes a managed `reading-inbox-<volume-uuid>.json` wrapper containing the exact native records (`format: reader-workbench-native-v1`, `device_id`, `annotations`); that is the input for the explicit website launcher. A zero-note card creates no native JSON input, and existing managed exports are not deleted. Existing files are only rewritten when content changes; a non-managed file at either path is refused. Sync does not delete card books, create a missing card without cause, or run any firmware action.

The site route and card route use the same `/books/Inbox/<basename(source_path)>` convention and the same deterministic collision names. Identical bytes at the canonical path are already delivered. If the canonical path holds different bytes, card sync uses `<stem>-<sha256[0:8]>.epub`; if that also differs, it uses `<uuid>.epub`, and only an exhausted ladder stops without overwriting anything. An identical file at any rung is treated as delivered. A small local ledger under the cache records each cache item's installed filename and a `pending`/`complete` state. Pending entries are persisted before `library.dirty`; the marker is made durable next, and only then are entries marked complete. A retry after interruption therefore marks the library dirty even though the bytes already match. Repeated completed entries skip the marker, so a book delivered through both routes is not duplicated.

Use exactly one annotation uploader per deployment. If the card's `/.crosspoint/reader-sync.json` has direct device sync enabled, the firmware uploads annotations; do not also run `cloud sync --annotations-in` against that card. A pre-existing `Sync website.command` is for the companion-managed upload path, not a second uploader for a direct-sync card.

### Enrich books: sharpest-passage front cards and Cross-Fire

```sh
# One-page "Sharpest Passages in This Book" card at the front of a real EPUB.
reader-send hooks ./Grey-Eminence.epub --dry-run
reader-send hooks ./Grey-Eminence.epub -o ./grey-hooked.epub

# Pair the card's annotations with replies from the local Substack archive.
reader-send crossfire --dry-run
reader-send card sync --crossfire      # or: card autosync --crossfire
```

`hooks` reads the EPUB spine, skips covers, title pages, indexes, copyright, bibliographies and similar apparatus, and scores body paragraphs/sentences for claim density, contrast markers and aphoristic punch. It selects four or five self-contained passages from different chapters, adds an `id` anchor to each source paragraph (never overwriting an existing id), and inserts `reader-hooks-sharpest.xhtml` at spine position 0 with one tappable link per passage pointing at `chapter.xhtml#anchor`, so the reader jumps straight into the argument. The card carries a `reader-hooks:sharpest:v1` marker; a rerun replaces the same card and produces byte-identical output instead of duplicating it. In-place writes keep a `<book>.pre-hooks.epub` backup unless `--no-backup` is given. Books without enough strong prose are refused rather than padded.

On the card side, `card sync` and `card autosync` copy the enriched book from the `~/papyrix/books` shelf into `/books/Books/` and ignore the `.pre-hooks.epub` backup (see the shelf rules under "Convert Chinese TXT novels for the reader"). `--crossfire` writes its `crossfire-<hash>.epub` card into `/books/Inbox`, where the Inbox cleanup pass leaves it alone because it is not a synced feed item.

`crossfire` reads the imported native annotations (highlight, question, review-later), tokenizes the highlight plus note, and searches the local archive at `~/substack-corpus` (Markdown) and `~/substack-neighbors/posts` (JSONL). The archive is chunked into paragraph-sized passages and kept in a persistent SQLite FTS5 index whose rows are refreshed only for changed files by path+mtime+size signature; the first build over the full archive takes a few minutes and several hundred MB under `~/.cache/reader-send/crossfire.sqlite3`, later runs only re-sign files. Without FTS5 the same query uses an in-memory BM25 index over pre-filtered posts. Each annotation gets two or three sharp, non-duplicate passages from preferably distinct authors, plus the original author, essay title, date, URL and excerpt, in a single `Cross-Fire: Marginalia & Replies` EPUB written to `books/Inbox/`. The filename is the content hash, so rebuilds that change nothing write nothing; `card sync --crossfire` reports the result under `annotations.crossfire`, and a retrieval failure never blocks delivery of the books themselves.

### Automatic delivery when the card is plugged in

```sh
reader-send card autosync            # what the LaunchAgent runs; safe to run by hand
reader-send card autosync --no-notify --batch 8 --passes 1
```

`card autosync` is the SD-card fallback while Wi-Fi sync is unavailable. It resolves the configured volume UUID first; when that exact card is not mounted (any other volume, or none) it logs one `skip:` line and does nothing else — no key read, no request, no write, no notification. For the configured card it takes a non-blocking lock in the cache directory (a concurrent run exits as `busy`) and then, in order:

1. lists the ready cloud items and plans the pass inside the server's 60-requests-per-minute allowance (minus a 4-request margin): listing pages, downloads and acknowledgements all count. Books already in the cache cost one acknowledgement each and are handled first; the requests that remain pay for up to `--batch` new downloads (1–27, default 27; a download plus its acknowledgement costs two) into the cache; each download is already SHA-256/size/mimetype checked by the cloud client;
2. copies **only those books** (not the rest of the cache) into `/books/Inbox` with the ordinary `card sync` rules — no overwriting, deterministic collision names, `library.dirty` marker — and imports card annotations into the annotation folder from `card.json`;
3. re-reads every card copy it is about to acknowledge from the medium (uncached, symlinks refused) and compares its SHA-256 with the cloud item's checksum; a copy whose SHA-256 matched on an earlier run is not re-read while its size and modification time are unchanged (a changed size or time always re-reads it, and a fresh copy is always read back);
4. acknowledges, one request each, **only the items whose card copy matched**, until the pass's request budget is spent; an item that failed verification stays `ready` on the server and is reported as held back, and items beyond the budget are acknowledged on the next pass or mount;
5. if more ready items remain it waits 60 seconds and repeats, up to `--passes` (1–10, default 3), which keeps every minute under the server's 60-request budget.

HTTP 429 stops the run immediately with no retry: acknowledgements already made stand, everything else is retried by the next mount (a card copy that is already verified is acknowledged without downloading again, ahead of any new download). Annotations are imported even when the cloud step fails or the key is missing. The same run also fills the two local shelves, with the `card sync` rules below and only when their source folders exist: the converted novels (`~/papyrix/novels-epub`, when it has a manifest) into `/books/Novels/`, and the local shelf folder (`~/papyrix/books`) into `/books/Books/`. Shelf books are copied, never moved, and a shelf error makes the run exit 2 without withholding the cloud books. The run never ejects and never stages firmware. It deletes no book: the Inbox cleanup step below only *plans* moves into `/books/Inbox/.trash/` unless `"inbox_cleanup": true` is set in `card.json`. It appends a few lines per run to `~/Library/Logs/reader-card-autosync.log` (rotated at 512 KiB; never contains the key) and posts one macOS notification with the number of books delivered. Exit status is 0 for success, skipped or busy, and 2 for an error, a shelf error or rate limit.

The management key is **not** stored in a plist, script or log. `READER_SYNC_KEY` is used when it is in the environment; otherwise the run reads the dedicated Keychain item `reader-workbench:sync-key`, account `b` (the same item `collect` uses) at run time and holds the value only in the process, restoring the environment afterwards. To (re)store it, paste the 64-hex key at the prompt (no key on the command line):

```sh
security add-generic-password -U -a b -s reader-workbench:sync-key -w
```

The LaunchAgent is `~/Library/LaunchAgents/dev.reader-workbench.card-autosync.plist` (label `dev.reader-workbench.card-autosync`). It has `StartOnMount` and `RunAtLoad` and runs `<repo>/companion/.venv/bin/python -m reader_send --cache-dir ~/.cache/reader-send card autosync`, with no secrets and no schedule: launchd starts it on every volume mount and the UUID check makes every other volume a no-op. Load it with `launchctl bootstrap gui/$(id -u) <plist>`; `launchctl bootout gui/$(id -u)/dev.reader-workbench.card-autosync` removes it. It is independent of the (disabled) `reader-send-subscriptions` agent. macOS may ask once to allow Python to access removable volumes; allow it.

### X daily digests, Inbox repack and Inbox cleanup

X posts reach the reader as **one EPUB per local day** instead of one book per post. The server (`example.com`, `server/digest.js`) builds them, so Wi-Fi direct sync and card autosync both receive the same files and the firmware needs no change:

- A post whose `original_url` is an X status URL is held back from the `ready`/`active` listings until its digest exists. An X post shared explicitly through `/reader/links` is still delivered on its own.
- Every listing, management (the collector every 15 minutes, card autosync) or device, does at most one small unit of feed work, so each request stays within a Workers Free CPU budget (10 ms). A unit is one of the following:
  - describing one uploaded item that is still undescribed;
  - planning one volume;
  - rendering one posts file;
  - writing the front matter (feed.json, OPF, navigation, contents);
  - sealing the volume.

  Undescribed uploads are always described before any day is planned. A day (America/New_York, server env `READER_DIGEST_TZ`) is published once it has ended and the local clock has passed 05:00 (`READER_DIGEST_HOUR`). Each part is stored in D1 with its CRC-32 and size. The digest is served as a STORED (uncompressed) zip assembled from those parts, with no compression or CRC work per request, and a rebuild produces identical bytes.
- Server volumes hold at most 100 posts or 512 KiB of post markup. Server digests are text-only: a photo becomes a `[photo]` link to the post. The firmware checks a whole-file SHA-256 before acknowledging, and computing that over images does not fit the per-request budget.
- Titles: `X — Sun 2 Nov 2026`, then `X — 2 Nov (2)`, `(3)` …. Posts that arrive after a day's volume was delivered go into the next volume number; a built volume is never rewritten.
- **Layout.** Posts flow like a feed, with no page breaks. Each post is a `<div class="post" id="p-<key>">` block:
  - name, `@handle` and local time in small bold;
  - then the text, quoted post or thread replies;
  - then a thin `<hr/>` rule before the next post.

  Whole author groups share spine documents `OEBPS/posts/NNNN.xhtml` of at most 25 posts and 48 KiB; a bigger group is split. The NCX and nav point at whole posts files, never at post anchors, because the reader starts a new page at every TOC anchor.
- A re-uploaded copy of a post already in a volume joins that volume rather than being delivered twice.
- Acknowledging a digest consumes every member post in the same transaction, once; a repeated acknowledgement is a no-op. Deleting an undelivered digest returns its posts to the queue.
- Articles that have an author (Substack, RSS) stay one EPUB each but are listed under the card name `Author — Title (2 Nov 2026).epub`. The stored `source_path` is unchanged, so the collector's metadata comparison still holds.

**Feed manifest (`reader-feed-v1`).** Every digest contains `META-INF/feed.json`, referenced from the OPF by `<meta name="reader-feed" content="META-INF/feed.json"/>`. A future on-device feed screen can read it instead of parsing HTML:

```json
{"format": "reader-feed-v1", "kind": "digest", "id": "<digest item id or null>", "source": "x",
 "day": "2026-11-02", "volume": 1, "timezone": "America/New_York", "title": "X — Mon 2 Nov 2026",
 "generator": "server",
 "items": [{"id": "<cloud item id or null>", "key": "x:<status id>", "source": "x", "author": "Ann A",
            "handle": "ann", "published": "2026-11-02T14:05:00Z", "url": "https://x.com/ann/status/…",
            "href": "posts/0001.xhtml#p-x:<status id>", "title": "first 140 characters"}]}
```

- `href` is relative to `OEBPS/`: the posts file plus the post's anchor `p-<key>`. Items are in contents order: grouped by author, then by time. Inside a posts file, the i-th item with that file is the i-th `body/div`.
- `OEBPS/contents.xhtml` is the first spine item and links every post by its `href`. `key` is stable across re-uploads and repacks. `id` is null only for digests repacked from card files with no cloud id.
- `generator` is `"server"`, `"companion"` or `"companion-repack"`. All of them write the same fields and layout; only companion builds embed images.

**Annotations in digests.** The firmware still uploads `item_id: null` with `book_path` set to the card path. Its `source_anchor.native` holds `chapterHref` (the posts file) and `xpath` (the page start, `/body/DocFragment[n]/body/div[i]…`).

`GET /reader/annotations/feed?after=&limit=` (management key only) resolves each annotation to `{sequence, annotation_id, digest_id, item_id, key, href}` in four steps:
1. Match the card name, including the firmware's `-<sha8>.epub` collision name and `<id>.epub`.
2. Pick the manifest items in `chapterHref`'s posts file.
3. Take the page-start post `div[i]`.
4. If the annotation has a selection, use the first post from there on whose text contains its first 64 characters (whitespace squashed); otherwise keep the page-start post.

The existing `/reader/annotations` records are unchanged. Repacked digests are named `… (card).epub` and are not known to the server, so their annotations stay unresolved (`digest_id: null`) rather than being matched to a server volume with the same title. Their `feed.json` carries the member ids for the same file + `div[i]` mapping offline.

**Repacking the singles already on the card** (dry run by default; prints one line per file):

```sh
reader-send card repack --root "/Volumes/NO NAME"          # plan only: N singles → M digests
reader-send card repack --root "/Volumes/NO NAME" --apply  # write digests, move singles away
```

- It reads each companion-format X post EPUB in `/books/Inbox` and groups them by local day (`--timezone`, default America/New_York) into digests named `X — Sun 2 Nov 2026 (card).epub`, with images kept inline. Digests already on the card, from the server or an earlier repack, continue their volume numbers and skip posts they already contain.
- `--apply` writes each digest (temporary file, fsync, rename, SHA-256 read-back), then moves the originals to `/books/Inbox/.singles-archive/` without deleting anything. It drops their `recent.json` entries and marks the library dirty.
- Singles that have highlights or notes stay where they are. Without `--root`, the configured card (`card.json`) is used.

**Inbox cleanup** (auto-delete) runs at the end of every `card autosync` and can be run by hand with `reader-send card cleanup [--root PATH] [--apply]`. It is a dry run unless `--apply` is given or `card.json` has `"inbox_cleanup": true` (absent = off). The policy:

- X digests and single X posts move to trash 3 days after they were finished, or 30 days after delivery even if unread. Delivery time is the file's modification time, or when cleanup first saw the file.
- Articles move to trash 14 days after they were first seen finished. An unread long post is never removed, and neither is a non-X book that did not come from sync.
- "Finished" comes from the reader's `progress.bin`: past the last spine item, or on the last page of the last one.
- A book with highlights or notes is held until every one of its annotations has a server receipt (`/.crosspoint/reader-sync/notes/<id>.receipt`).
- Annotations stay retrievable after cleanup: on the server (`reader-send cloud annotations`, `/reader/annotations/export`) and on the card, where `/.crosspoint/annotations/<key>/` is never touched.
- "Delete" moves the book to `/books/Inbox/.trash/` and removes its reading cache (`/.crosspoint/epub_<hash>`) and its `recent.json` entry. Trash entries older than 7 days are purged.
- State lives in `<cache>/.inbox-cleanup-<volume uuid>.json`, and repeated runs are idempotent.

### Convert Chinese TXT novels for the reader

```sh
reader-send novels convert SRC_DIR OUT_DIR [--jobs N] [--force] [--rebuild-stale] [--json]
reader-send card sync [--novels OUT_DIR] [--books DIR]
```

The reader builds a book's page index one spine file at a time, so a multi-megabyte `.txt` takes tens of seconds to open while the same text split into chapter files opens in seconds. `novels convert` walks `SRC_DIR` recursively (hidden files and folders are skipped; `.txt` only, case-insensitive), writes a mirrored `.epub` tree below `OUT_DIR`, and **only ever reads `SRC_DIR`**: an `OUT_DIR` equal to or inside `SRC_DIR` is refused before anything is created.

- **Decoding.** UTF-8 with or without BOM, UTF-16 with a BOM (either byte order), then strict GB18030. GB18030 text with embedded UTF-8 punctuation (bullets from some exporters) is recovered and flagged in the report's `warnings`. A file that decodes in none of these, or that is NUL-heavy (binary data, or UTF-16/32 without a BOM), is **refused with a reason**; replacement characters are never inserted. Isolated stray NUL bytes (extraction debris, under 0.1% of the text) cannot be stored in XML, so they are removed and counted like other illegal control characters.
- **Chapters.** Headings are `第…章/节/回/卷/集/部/篇/话` with Chinese, Arabic or fullwidth numerals; `序章 楔子 引子 前言 尾声 番外 后记 完本感言 Prologue Epilogue`; `Chapter N` / `1、Chapter 1`; `###第1章，…###` and `###《…》###` wrappers; Jinjiang `☆、…` lines; Markdown `##`/`#` lines when the file clearly uses them (three or more `##` lines); and bare `一 标题` / `077，标题` / `836标题` lines when they form a 1, 2, 3… chain of at least ten entries that starts at 1–3, tolerates up to nine missing numbers in a row, and is not a crowded list (forum floors, outlines); a contents block at the top never wins over the real chapters. A heading directly followed by another heading (a table of contents at the top of the file) stays plain text instead of becoming empty chapters. Volumes (`卷/部/集`) nest their chapters in both the EPUB 3 nav and the NCX; text before the first heading becomes its own `卷首` file.
- **Fallback.** Fewer than five headings → the book is cut into size-bounded files labelled `第N部分`. A chapter larger than 56 KB is cut at paragraph boundaries into continuation files (spine only, one TOC entry; a chapter cut into eight or more files gets a TOC entry `标题（N）` per file so it stays navigable); a single paragraph over 56 KB is cut at sentence ends. Every XHTML file stays under 60,000 bytes; the aim is ~40 KB.
- **Paragraph style.** Leading ASCII spaces, tabs and U+3000 are stripped from every paragraph and blank-only lines are dropped; each remaining source line is one `<p>`, indented by the stylesheet (`p{text-indent:2em;margin:0}`), with volume/chapter headings unindented (`h1`/`h2`) and shown without decoration (see below); number echoes (see below) are `h3`. On the reader, turn extra paragraph spacing off to show the positive first-line indent; spacing on intentionally uses flush-left paragraphs with space between them.
- **Metadata.** `dc:language` is `zh-CN`; title and author come from `[作者]书名.txt`, `《书名》作者：X.txt`, `书名_作者.txt`, `书名（作者：X）.txt` and similar, falling back to the stem and to an `作者：` line in the file's first paragraphs. Output names are NFC, FAT-legal and short enough for the reader's library index. The EPUBs are byte-for-byte reproducible.
- **ZIP startup layout.** Package metadata, navigation documents and the stylesheet precede chapter members. Readers that scan the ZIP central directory avoid walking thousands of chapter entries for each startup metadata lookup. This does not eliminate whole-book indexing or late-chapter ZIP lookups.
- **Large-book grouping (converter v10).** Books with more than 3,000 detected chapters pack consecutive whole chapters into size-bounded XHTML files, keeping the 60,000-byte ceiling. Only chapters already too large for a file use the existing continuation splitting. Each chapter heading has a unique fragment id, and both nav and NCX retain one entry per chapter targeting `file.xhtml#id`; volume nesting and long-chapter continuation entries remain intact. Fragment targets give the reader fresh chapter-start pages while fewer spine items reduce book-cache heap pressure. Books with at most 3,000 chapters remain byte-identical to v9, including their EPUB identity, and unchanged v9 manifest entries for those books are skipped rather than reconverted.
- **Nothing is lost silently.** Each EPUB is re-read before it is written: every non-whitespace character of the spine, in order, must equal the normalized source text with only the documented removals applied (newlines unified; the removals are XML-illegal control characters, which are counted, and the decoration below). Zip layout, XHTML well-formedness, the 60 KB file ceiling and nav/NCX targets are checked too. A book that fails is not written and is reported as failed.
- **Decoration.** On heading lines the leading and trailing `#`, `=`, `-`, `*`, `☆`, `★`, `※` and spaces are not displayed (`## 第1章 标题 ##` → `第1章 标题`; the comma of a Jinjiang `☆、` marker goes with its star), and lines made only of those characters (`====`, `----`, `****`, `☆☆☆`, `* * *`) are dropped. The same characters inside a heading or in an ordinary paragraph are kept. The check derives the expected text from the source by that same rule and requires each removed character to be a decoration character at the edge of a detected heading or part of a decoration-only line; anything else missing fails the book. Per book, `removed_decoration_chars`, `decoration_lines_dropped` and `headings_cleaned` are recorded in the manifest and report, with totals in the report summary. Books without detected headings (size mode) keep marker characters in their text but still lose decoration-only lines.
- **Echoes.** Some sources print a chapter's number again right under its title (`## 第二章 灭世重生` / `====` / `第002章`). A heading that repeats the chapter number (same unit, same value; Chinese, Arabic and fullwidth numerals and zero padding are equal; a short prefix such as `正文_` is ignored) of the heading before it, with only blank or decoration-only lines between them, is not a chapter of its own: the titled heading stays the chapter and its TOC entry, and the repeat is shown as a heading-styled `<h3>` line at the top of the same file. The text is kept, so the preservation check is unchanged. A repeat separated from its title by any body text (a `PS：` note, a site advertisement) is not an echo. A heading that has nothing but echoes after it is still a bodiless heading and stays plain text.
- **Duplicate titles.** An echo line whose visible characters equal the heading it follows (the same title printed twice in a row, as in some site exports where `第79章 ，夺宝` is followed by `第79章 ，夺宝`) is not shown a second time: spacing between the two lines is ignored, so `第79章，夺宝` after `第79章　　夺宝` is the same title, and the chapter number is read as a number, so `第二百章 布拉德` under `第200章 布拉德` is the same title too. A second copy of a title already shown under the same chapter is dropped as well. It is dropped and counted in `duplicate_headings_dropped` (lines) and `duplicate_heading_chars_removed` (its non-whitespace characters); these are never part of `removed_decoration_chars`, which keeps its meaning. The preservation check requires source length = shown text + decoration removals + duplicate characters, so nothing else can vanish. Both counters are in the manifest and the report summary. An echo that differs in any character, spacing aside, stays as the `<h3>` line. Source spacing, no-break spaces and invisible characters are not rewritten. Manifest entries of newly built books carry `text_clean` as provenance only: existing entries without it are still skipped when the source is unchanged, and are rebuilt only by `--rebuild-stale` or an explicit `--force` (or a source change).
- **Idempotent.** `OUT_DIR/.novels-manifest.json` records each source's SHA-256, the converter version and the `text_clean` revision of the text handling that built it; unchanged books with an existing output are skipped, failed books are retried, `--force` rebuilds everything, and manifest entries whose source disappeared are reported as `orphaned` but their EPUBs are never deleted. `--rebuild-stale` reconverts exactly the entries built by an older text handling (a missing or older `text_clean`) without touching the current ones, which is how the corpus is brought up to date after a converter change that alters the XHTML rather than the source fingerprint. `OUT_DIR/.novels-report.json` holds the full per-book report (status, encoding, mode, volumes/chapters/files, warnings, errors); `--json` prints it, and per-book progress goes to stderr. The exit status is 2 when any book failed.

`card sync` also fills the reader's two local shelves; `--novels OUT_DIR` and `--books DIR` point them elsewhere. Both plans are validated before the first card write and both ask for a library rescan with the usual `/.crosspoint/library.dirty` marker. `--novels` copies the converted Drive novels (default `~/papyrix/novels-epub` when it holds a `.novels-manifest.json`) into `/books/Novels/` (subfolders are mirrored); a missing manifest or a symlinked card folder aborts with nothing written, and an EPUB that no longer matches its recorded SHA-256 (or whose card path is too long for the reader) is skipped and reported. `--books` copies every `.epub` below one local shelf folder (default `~/papyrix/books` when it exists) into `/books/Books/`, keeping relative subfolders, deduplicating identical files by content, and reporting — never following — symlinked books. On both shelves copies are fsynced, never overwrite, and are read back and compared with the source hash; a bad copy is removed and reported (exit status 2) while the other books still land. Nothing is moved: the Mac's files stay where they are. Existing card files are never deleted; if the same name already holds different bytes, the new EPUB is installed as `<stem>-<sha256[0:8]>.epub`. Reconverting a changed source therefore adds a hashed copy and leaves the old one for you to remove.

`--books` and `card autosync` never deliver `*.pre-hooks.epub` files: that is the name of the original copy `reader-send hooks` keeps beside a book it enriched in place, and delivering it would put the same book on the card twice (once without its front card). The enriched book is copied; the backups stay on the Mac. Keep the backups outside the shelf if you prefer a tidy folder (`~/papyrix/books-pre-hooks/` is where the real shelf's originals live).

### Stage reviewed firmware only

```sh
reader-send card update
# Or explicitly choose both an image and its expected hash:
reader-send card update --image ./other-reviewed.bin --expected-sha256 'FULL_64_HEX_SHA256'
```

`card update` verifies the local image's SHA-256 **before** touching the card, stages exactly one file at the card root as `reader-workbench-rollback-reviewed.bin` (the reviewed basename; an explicitly supplied image is also staged under this name after its hash matches), flushes/atomically replaces it, and re-hashes the card readback. The picker accepts any `.bin`, so this keeps one current reviewed candidate and its source hash pinned without adding a second candidate. An identical already-staged image reports `staged: false`; no builder, release downloader or automatic firmware update is provided.

The picker lists every visible `.bin`, so select `reader-workbench-rollback-reviewed.bin` explicitly rather than assuming the first entry. On the reader use:

**Settings → System → SD card firmware update** → **Select firmware file (.bin)** → choose `reader-workbench-rollback-reviewed.bin` → confirm **Update firmware?** → **Do not power off** while it validates or writes.

The updater writes the inactive application slot. This can replace stock; after updating, fallback is the previous custom firmware version, not perpetual stock. It does not rewrite the bootloader or partition table. `reader-send` never flashes, reboots, ejects, or selects a slot automatically. Keep the card mounted on the Mac while staging, then eject it yourself before inserting it in the reader.

### Optional firmware direct sync

`card provision` writes only the firmware's explicit direct-sync config at `/.crosspoint/reader-sync.json`; it never stages firmware or touches books/annotations. It reads the dedicated device key from the Keychain service `reader-workbench:device-key` / account `b` and never prints it. An existing valid config is preserved, including its key, unless you explicitly use `--rotate-key`; use `--enabled` or `--disabled` to change only the enabled state. A fresh card gets the six original fields (the reader then defaults to its event-driven schedule); an optional `"schedule": "events"|"interval"` that the reader wrote from Settings → Reader sync → Schedule is validated and preserved. Firmware from before the event-driven schedule rejects a file that has a `schedule` key, so delete that key before rolling back.

```sh
reader-send card provision --enabled
reader-send card provision --disabled
reader-send card provision --rotate-key --enabled
```

If an existing config is invalid, provision refuses to replace it without `--force`. Use only one annotation uploader: if this direct-sync config is enabled, do not also feed the card's native records into `cloud sync --annotations-in`.

### Schedule recent collection and private site sync

After adding chosen publications and starting X Following:

```sh
reader-send subscriptions add essays --feed 'https://publication.substack.com/feed'
reader-send subscriptions schedule install   # optional --config PATH for the card configuration
# One manual pass with the same pipeline; no card configuration is read:
reader-send collect
# Or pin the annotation destination explicitly:
reader-send collect --annotations-out "$HOME/papyrix/annotations/cloud-annotations.ndjson"
```

`subscriptions schedule install` reads the card configuration **once, at installation time** (`--config`, default `~/Library/Application Support/reader-send/card.json`) and writes the resulting destination into the plist as explicit `collect --annotations-out <annotations-dir>/cloud-annotations.ndjson` arguments, so the current intended destination keeps working without the collector depending on the card. If that configuration does not exist, the pinned destination is `<cache-dir>/annotations/cloud-annotations.ndjson`. A manual `collect` without `--annotations-out` uses that same cache-root default and never reads card configuration.

The install hook writes and loads `~/Library/LaunchAgents/dev.reader-workbench.reader-send-subscriptions.plist` with `StartInterval` 900 seconds and `RunAtLoad` true. Each pass runs `subscriptions sync --since-days 7` and `x-following sync --since-hours 24 --limit 50`, drains up to 8 pending shared links through the normal public fetch or authorized X browser adapter plus verified push flow, then runs `cloud sync --drain --annotations-out <pinned path>`: it pushes newly collected cache entries through the private site, pulls ready books into the cache, and pulls reader annotations to that managed NDJSON file. The admin key is read from the `READER_SYNC_KEY` environment variable first; only when that variable is **absent** may macOS fall back to the dedicated Keychain item `reader-workbench:sync-key` / account `b`. A present-but-invalid `READER_SYNC_KEY` fails that collector with an explicit error and is never replaced by the Keychain value, and on hosts without Keychain it must be supplied. The reader's restricted device key is separate and never stored on the Mac. A nonblocking collection lock prevents overlapping launchd passes; the manual `collect` command uses the same pipeline. If a scheduled publication uses `--browser-url` or X Following, the already-running Chrome DevTools session must remain available; otherwise that collector reports the browser error and the job exits 2 without silently falling back to RSS. The job does not stage firmware, flash, reboot, or run assist, and it does not install itself during setup. SD delivery remains the manual `card sync` fallback and reviewed firmware staging remains the manual `card update` path; neither is run automatically by `collect`. Logs are under `~/Library/Logs/reader-send-collect*.log`. Remove the plist (and boot it out with `launchctl bootout gui/$(id -u)/dev.reader-workbench.reader-send-subscriptions`) to stop scheduling.

### Independent collector on the always-on Windows laptop (WSL)

The same `collect` pipeline also runs without the Mac, on Robert's existing always-on Windows laptop inside WSL Ubuntu (`ssh windows`, user `bob`). This is **not** an independent cloud service: it runs only while that laptop has AC power, is awake, connected to the network and has WSL running. When the laptop or WSL is down nothing is collected; the timer's `OnCalendar` with `Persistent=true` (which, per `systemd.timer`, only applies to calendar triggers) starts one catch-up pass when the timer is next loaded, and the 7-day / 24-hour collection windows (with `window_gap` reporting for X) bound what can be recovered. No Windows wake, power or startup policy was changed.

- Units (system, `User=bob`, installed with `wsl -d Ubuntu -u root`): `/etc/systemd/system/ops-reader-collect.service` (oneshot, `WorkingDirectory=~/reader-collector`, `ExecStart=~/reader-collector/bin/run-collect`, `TimeoutStartSec=1800`) and `/etc/systemd/system/ops-reader-collect.timer` (`OnBootSec=3min`, `OnCalendar=*:0/15` i.e. every quarter hour, `AccuracySec=1min`, `Persistent=true`; a tick that arrives while a pass is still running does not start a second one). Verified with `systemd-analyze calendar '*:0/15'` and `systemd-analyze verify`.
- Layout under `~/reader-collector/`: `app/` (source snapshot), `venv/` (`uv sync --frozen --extra browser --no-dev --no-editable`), `state/cache/` (its own cache; the 39 subscription definitions were copied from the Mac, no EPUBs, X seen-list or refresh state), `annotations/cloud-annotations.ndjson`, `chrome-profile/` (dedicated), `logs/collect.log` (rotated at 8 MiB), `bin/run-collect` and `bin/login-profile`.
- Key: `~/.config/reader-collector/sync-key` (mode 600, owner `bob`, one 64-lowercase-hex line) was written by piping the Mac Keychain item `reader-workbench:sync-key` over ssh stdin; it was never put in argv or printed. `run-collect` exports it as `READER_SYNC_KEY` only to the `reader-send collect` child. Rotate it the same way.
- Browser: each pass `run-collect` starts a headless Chrome on `127.0.0.1:9333` over the dedicated profile and stops it afterwards. The port is loopback inside WSL (NAT) and is not exposed to Windows, the Mac or the network. Chrome runs with a normal desktop user agent because x.com answers `HeadlessChrome` with HTTP 403; sign-in state still comes only from the profile. A `flock` on `chrome-profile.lock` makes the pass skip (exit 0, `skipped` JSON) while the login window below is open.
- One-time sign-in, on the laptop's desktop: `wsl -d Ubuntu -- ~/reader-collector/bin/login-profile`. It opens headed Chrome (WSLg) on the dedicated profile at `https://x.com/i/flow/login` and `https://substack.com/sign-in`; sign in by hand, then close the window. Nothing is copied from the Mac or from any other Chrome profile.
- Until that sign-in exists, X Following and the Substack-only sources (`alreadydawn`, the Substack settings refresh) fail with explicit sign-in errors, `collect` exits 2 and the unit shows `failed`. Public RSS, shared links, push/pull and annotations still run. **Remote X Following and Substack-only collection are not proven**, so the Mac LaunchAgent stays loaded as the fallback. Both collectors can run together: article IDs are deterministic and the cloud deduplicates.
- Operate from a Windows shell: `wsl -d Ubuntu -u root -- systemctl list-timers ops-reader-collect.timer --no-pager`; `wsl -d Ubuntu -u root -- systemctl start ops-reader-collect.service`; `wsl -d Ubuntu -u bob -- tail -n 80 ~/reader-collector/logs/collect.log`. Each run is one JSON document between the `run-collect start` and `run-collect end exit=N` lines. To update, replace `app/` from a fresh `src`/`pyproject.toml`/`uv.lock` snapshot and rerun `uv sync` with `--reinstall-package reader-workbench-companion`.
- Observed on 2026-10-31: the Mac agent was booted out 17:40:40Z–17:48:18Z and restored. A remote `cloud sync --max-push 8 --max-pull 1` pushed 8 / already synced 23 / conflicts 0 and pulled 1 plus annotations, and a separate signed `cloud list --all` from the Mac listed all 8 item IDs, one of them newly created by the remote at 17:47:31Z. The first enabled timer pass (13:48 EDT, 4 min 21 s) collected public RSS and shared links without errors and pushed 2 / pulled 24 books before a drain batch stopped on HTTP 410 for a book another client had just acknowledged; X and Substack-only sources reported the missing sign-in.
- Current-tree deployment, 2026-10-31 (finished before the 18:44Z smoke): `app/` was replaced from a checked staged copy (extracted to `app.new`, required files verified, no suppressed failures, refused while a pass was running) and reinstalled with `uv sync --reinstall-package reader-workbench-companion`; the manifest hash of every installed `reader_send/*.py` equals the local tree (`73dd8f4848bb…a8bacab`). A read-only production smoke on that WSL venv (no ACK, push or delete) listed 506 items (256 consumed / 250 ready): pulling a consumed item raised `CloudContentReleased` and left no file, and pulling a ready item verified its size and SHA. Consumed stayed 256; ready rose to 251 because another client uploaded meanwhile. The 14:30 EDT pass (13 min 53 s, exit 2 for the missing X/Substack sign-in) still ran on the previous build.
- First pass on the current build, 14:45 EDT (1 min 16 s, exit 2 for the missing X/Substack sign-in): public RSS and shared links ran without errors, and the cloud drain finished in one batch (pulled 7, 0 released, no conflicts). Its active-only listing reached the existing 256-ready-item ceiling; it did not count consumed receipts. A later UUID-bound SD delivery verified each file's size/SHA-256 and flushed it before acknowledging 345 more items, without DELETE calls or annotation uploads. At 19:18Z the card held 601 verified books and the include-all cloud listing held 601 consumed receipts plus one newly arrived ready item. This relieved the backlog; it does not claim that future uploads are already on the card. Evidence: `verification/adaptive-perf-nxtmx4fe/card-backlog-followup.jsonl`.

### Card tests

```sh
PYTHONPATH=companion/src companion/.venv/bin/python -m unittest discover -s companion/tests -p 'test_card.py' -v
```

The permanent card tests cover absent/wrong/internal/read-only volume refusal with no writes, verified EPUB delivery and repeat idempotency, interrupted delivery with a pending ledger that must still create `library.dirty`, hashed collision naming without overwriting different card bytes, annotation anchor preservation and unchanged-file handling, and firmware hash rejection before staging plus same-image idempotency. They use temporary files and a fake `diskutil` result; they never touch the real card.

```sh
PYTHONPATH=companion/src companion/.venv/bin/python -m unittest discover -s companion/tests -p 'test_card_autosync.py' -v
```

`test_card_autosync.py` drives the real cloud client against a fake HTTP transport and a plain folder as the card: delivery of only the cloud books with byte-identical copies, ack only after the card copy verifies (a corrupted copy stays unacknowledged), idempotent rerun, wrong/absent volume doing nothing at all, HTTP 429 on the listing, during a pull and during an ack with clean recovery on the next run, annotation import with no key, the concurrency lock, multi-pass backlog and the command line end to end.

`companion/tests/test_companion_integration.py` runs the features together on one plain-folder card: a real hooked EPUB with its `.pre-hooks.epub` backup on the shelf (only the enriched book is delivered), `card sync`/`card autosync` with `--crossfire` plus the Inbox cleanup plan in the same mount, and a check that cleanup never trashes a Cross-Fire card. The opt-in real-data tests in `test_book_hooks.py` and `test_crossfire.py` read `RW_REAL_BOOKS`, `RW_REAL_CORPUS`, `RW_REAL_NEIGHBORS` and `RW_REAL_DB`.

`companion/tests/test_novels.py` covers the novel converter and its card delivery the same way (temporary directories only; the card is a plain folder):

```sh
PYTHONPATH=companion/src companion/.venv/bin/python -m unittest discover -s companion/tests -p 'test_novels.py' -v
```

## One-action X/Twitter post or thread

```sh
# Offline authorized export → reading inbox (+ optional one-action delivery).
reader-send x 'https://x.com/writer/status/123' --export thread.json
reader-send x 'https://x.com/writer/status/123' --export thread.json --send --reader 192.168.1.42:53317

# Your already signed-in Chrome → reading inbox (no API key, no export).
reader-send x 'https://x.com/writer/status/123' --browser
reader-send x 'https://x.com/writer/status/123' --browser --send --reader 192.168.1.42:53317

# Explicit official API access using your existing token in the process environment:
reader-send x 'https://x.com/writer/status/123' --api --output ./thread.epub
```

`--export` accepts official API v2 JSON: `data` is one tweet or an array, with optional `includes.users`, `includes.media` and `includes.tweets` for quoted/parent posts. Posts need `id`, `author_id`, `text` (or `note_tweet.text`), and `referenced_tweets` for connections; include `conversation_id` for stable identity across partial exports, `created_at`, `entities.urls`, and `attachments.media_keys` when available. This is **not** the JavaScript `window.YTD...` account archive format. Export-mode image downloads require `--remote-images`; otherwise conversion remains offline.

The adapter follows connected replies by the original author, orders them by post ID, preserves quoted context, expanded links, authors/dates, accessible pictures and video thumbnails. Other users' replies and disconnected author posts are excluded. Video/Spaces audio is not embedded: an original URL and preview notice remain in the EPUB; metadata contains `media_links` and a `qr_payload` URL for downstream QR rendering (no QR bitmap is generated). Missing/deleted/private posts cannot be reconstructed, and incomplete access is explicitly warned.

`--api` reads `X_BEARER_TOKEN` from the environment without saving it, calls only `api.x.com`, and refuses redirects that could expose the token. It uses one post lookup and at most three 100-result recent-search pages; X's access tier, recent-search window and possible API charges apply. It is opt-in, not a free/public endpoint promise. No automatic API retries are provided, and nothing runs on a timer or in the background. On access/quota failure, supply an authorized JSON export instead. Browser accounts/sessions remain Mac-side.

`--browser` reads the post from the Chrome window that is already signed in to X, through its local DevTools endpoint (`--browser-endpoint`, default `http://127.0.0.1:9333`; only loopback endpoints are accepted). Start Chrome once with the agent profile and keep it open:

```sh
open -na "Google Chrome" --args --user-data-dir=$HOME/.job-app-chrome --remote-debugging-port=9333 https://x.com/home
```

Sign in to X in that window, then pass one post/thread URL. The command creates one tab of its own, waits for the post page X renders, reads only the post data and rendered text that page loaded, and closes that tab again. Cookies, Authorization headers, other tabs and the rest of the session are never read, stored or forwarded, and no API key, export or paid API plan is involved. The result goes through the same conversion/cache/EPUB path as the other modes, with the same exclusions (connected author posts and quoted posts only, other people's replies dropped), the same long-form `note_tweet` full text, expanded links, dates, pictures and video thumbnails. When X renders only the first batch of replies, or when only the rendered page text is usable, the EPUB is still created with an explicit warning instead of claiming a complete thread. A signed-out Chrome window, a used-up thread, or an unreadable post fails with an actionable message and no cache write. This mode needs the optional DevTools dependency:

```sh
uv pip install -e '.[browser]'     # or: pip install 'reader-workbench-companion[browser]'
```

## Optional Mac passage assistance: Explain, Example, Quiz

`reader-send assist` is a separate, **explicitly invoked macOS-only** study aid. It sends just the selected excerpt, its note, source identity, and available source metadata to your chosen OpenAI-compatible chat-completions service. It never runs from collection/sync/send, reads book files or hidden prompt files, or installs a background process. Hosted services may charge and receive private book/note text: use only an endpoint you trust and content you are authorized to share.

Configure these exact **process environment** variables; no defaults or provider credential discovery:

- `READER_ASSIST_BASE_URL`: HTTPS API base, for example `https://YOUR-PROVIDER/v1`. The tool appends `/chat/completions` unless already present. A path prefix is allowed; credentials, queries, fragments, whitespace and traversal segments are refused. HTTP is allowed **only** for `http://127.0.0.1[:PORT]` or `http://localhost[:PORT]` local testing.
- `READER_ASSIST_MODEL`: the provider's explicit model ID (1–200 ASCII letters/digits or `. _ : / @ + -`, starting with a letter/digit).
- `READER_ASSIST_API_KEY`: only the dedicated environment variable, never a CLI argument, `.env`/config file, browser/keychain token, `OPENAI_API_KEY`, or cloud sync key. It must be nonempty ASCII without whitespace, at most 4,096 characters. `--dry-run` needs no key.

TLS certificate and hostname verification are mandatory; there is no insecure switch. There are **no redirects, cookies, proxy-environment lookup, automatic retries, credential persistence, or secret logging**. Error bodies are not printed. Responses reflecting the configured key are rejected before writing an artifact.

```sh
export READER_ASSIST_BASE_URL='https://YOUR-PROVIDER/v1'
export READER_ASSIST_MODEL='YOUR-MODEL-ID'

# Validate your explicit selection and configuration, without network or any writes.
reader-send assist explain ./native-annotation.json --dry-run

# In Bash (not zsh): inject the key without echo or a shell-history literal.
read -r -s -p 'Assist API key: ' READER_ASSIST_API_KEY
printf '\n'
export READER_ASSIST_API_KEY

reader-send assist explain ./native-annotation.json --output ./explanation.md
reader-send assist example ./one-entry-export.md --output ./example.md
reader-send assist quiz ./native-annotation.json --output ./quiz.md

# JSON collections, per-book Markdown and the all-source reading-inbox export:
# IDs are book-local, so use the ORIGINAL Source path to disambiguate repeats.
reader-send assist explain ./reading-inbox.md --entry-id 7 \
  --source-path '/books/optics.epub' --output ./optics-explanation.md
reader-send assist quiz ./annotations.json --entry-id 7 \
  --source-path '/books/optics.epub' --format json --output ./quiz.json
unset READER_ASSIST_API_KEY
```

Input must be one explicit regular file, not a directory or symlink. JSON accepts native records, record arrays, `{"annotations":[...]}`, and `.jsonl`/`.ndjson` via the existing annotation loader. API-shaped records with `selection`, `book_path`, and an opaque `source_anchor` (or `location` object) also work. Native JSON preserves **every original field**, exact excerpt/note whitespace and Unicode, original `sourcePath`, XPath, spine, chapter path, page hints, timestamp, unknown fields, and the distinction between absent `vo` and `vo: 0`.

Markdown accepts the native per-book export and `/annotations/reading-inbox.md` (`# Reading inbox`, `- Scope: All sources`). Entries use `## numeric-id`, per-entry `Source`, `Type`, `Chapter path`, `Spine index`, `XPath`, optional offsets/timestamps, `> excerpt`, and optional `**Note:**`. Select exactly one entry; ambiguity or a missing match fails before network. `(Source, id)`, **not id alone**, identifies a book-local record. Only the selected entry is sent/preserved, never the other inbox entries. Native Markdown escaping is decoded; the original selected export text and all exported location fields remain in the artifact. Markdown export has already normalized whitespace and rounded progress, and can omit native fields: assistance cannot recover these. Use native JSON for lossless native provenance. No referenced book, URL, adjacent entry, or other file is loaded.

Default output is a local `assist-ANCHOR-MODE.md` in the working directory; `--output` chooses the path. Existing files require `--force`; even with it, the input cannot be overwritten. The CLI prints a JSON summary of the artifact path and stable SHA-256 anchor. Markdown contains source, category, mode, configured model, generated UTC time, the exact supplied excerpt and note, and the **actual model answer**. Quiz output has actual questions, a separate answer key, and limitations; missing/empty question/answer data is rejected, not replaced with canned questions.

Both Markdown's fenced **Import provenance (JSON)** section and `--format json` use `reader-workbench-assist-v1`, preserving the original selected record and `source_anchor`. JSON also retains the model's answer and parsed quiz items. The stable anchor hashes source identity, full source location metadata and excerpt; it is unchanged across modes or repeated generation of the same capture. It is a source-snapshot anchor, not proof of a globally unique capture event or a cross-format merge identity. Generated time/model text do not affect it.

These are **local future-import artifacts, not native annotation writeback or cloud annotation uploads**. No reader or production storage is modified. Review model output for accuracy; the built-in prompt requires grounding in the supplied passage, hypothetical labels for examples, and explicit limitations instead of fabricated book facts. This is not full-book retrieval, and model compliance/accuracy cannot be guaranteed.

Limits fail closed without truncation: 12 MiB / 4,096 input records; 8,192 passage characters; 4,096 note characters; 32 KiB selected provenance metadata; 128 KiB HTTP response; 32 JSON nesting levels for response/selected metadata; 32,768 answer characters. Each explicit invocation makes one non-streaming chat-completions POST with `max_tokens: 2048`. Quiz accepts 1–5 question/answer pairs. Socket timeout defaults to 30 seconds (`--timeout`, at most 300), with a bounded response-read deadline. Truncated, refused, tool-call, malformed, oversized or non-JSON responses fail with no artifact. Network failures are not retried because a provider may already have charged.

Focused tests use a real loopback HTTP mock server (no live provider or credentials):

```sh
PYTHONPATH=companion/src python3 -m unittest discover -s companion/tests -p 'test_assist.py' -v
```

## Optional private cloud sync

Manual LocalSend/SD remain the fallbacks; the explicit user-installed `collect` schedule is the automatic private-site path. `reader-send cloud …` and that `collect` schedule are the **only** private reader-storage cloud opt-ins. There is no hidden background service: `collect` runs only from a timer you install (the Mac launchd job, or the WSL systemd timer above) or when you invoke it. It may fetch/convert recent sources, push/pull the private site, and pull annotations, but it never stages firmware, flashes, reboots, or runs assist. The protocol is documented in the portfolio repository's README under **Optional private reader sync**. The production backend is live at `https://example.com` (verified deployment `https://3cae2cd2.robertzhou.pages.dev`, project `robertzhou`, branch `main`, source `601a2c5`): additive migration `0008_reader_sync.sql` is applied, the Pages `READER_SYNC_KEY` secret is installed, and the companion's dedicated Mac Keychain item is `reader-workbench:sync-key` / account `b` (legacy `omp:reader-sync-key` retained in sync). A live signed API smoke on 2026-10-30 passed 15/15 checks with `User-Agent: ReaderSend/1`, including unsigned/wrong-key 401 JSON, signed list/reserve/upload/list/download with byte-identical EPUB and matching `X-Reader-Sha256`, ack, annotation create/list/NDJSON export, delete, and private/no-index headers; exact smoke rows were removed and pre-existing data was preserved. The companion client and `cloud sync` orchestration tests use in-memory transports, and no physical reader cloud transfer is claimed.

### Setup and security

- The production deployment already has the separate `READER_SYNC_KEY`: the encrypted Pages secret is installed, and this Mac's dedicated Keychain item is `reader-workbench:sync-key` / account `b` (the legacy `omp:reader-sync-key` value is retained and matches). The cloud client reads only the exact `READER_SYNC_KEY` environment variable: **64 lowercase hex characters**, representing 32 random bytes. HMAC uses those 64 characters as UTF-8 key bytes, **not decoded hex**. Do not reuse `OWNER_KEY`, `SIGNING_KEY`, browser credentials, or any other token.
- Bare `reader-send cloud` accepts no `--key`/secret CLI argument, secret file, `.env` loading, browser cookie, generic keychain lookup, or persisted secret. The exceptions are a pre-existing Finder **Sync website.command** and the scheduled `reader-send collect`. `collect` uses `READER_SYNC_KEY` from its own environment whenever that variable is present — as given, or failing clearly when it is not 64 lowercase hex characters — and only when it is absent on macOS reads the exact dedicated admin Keychain item `reader-workbench:sync-key` / account `b`. On hosts without Keychain the variable must be supplied. Either way the key reaches only the child process as `READER_SYNC_KEY`, is cleared after the call, and is never printed or persisted. The reader's dedicated restricted device key is separate and is not read by the Mac. For bare CLI use, inject the existing key into the process environment with a secure prompt or your secret manager; do not paste it into shell history. For example, in **Bash** (not zsh), `read -r -s -p 'Reader sync key: ' READER_SYNC_KEY; printf '\n'; export READER_SYNC_KEY` prompts without echo. Use `unset READER_SYNC_KEY` afterward. `cloud push --dry-run` needs no key; `cloud sync --dry-run` still needs it for its one listing.
- The default explicit origin is `https://example.com`. `--origin https://YOUR-HOST[:PORT]` intentionally authorizes that origin to receive both credentials and content; only use a trusted backend you control. HTTP, credentials-in-URL, paths, queries and fragments are refused. TLS certificate and hostname verification are mandatory, with no insecure switch. Standard-library HTTPS does not follow **any** redirects, send/store cookies, or consult proxy environment variables. There is no public-site robots/telemetry request. Requests identify as `ReaderSend/1`; live WAF compatibility with that exact UA was verified (JSON 401, not a challenge page), with no WAF bypass used or claimed.
- Each request signs `reader-sync-v1`, uppercase method, exact path/query, SHA-256 of exact body bytes, Unix timestamp and a fresh random 16-byte nonce, LF-separated with no final newline. JSON is serialized once and those same UTF-8 bytes are signed and sent. GET, DELETE and ack have empty bodies.
- System time must be within 300 seconds of the server. **No cloud request is automatically retried.** Rerunning a command creates fresh timestamps/nonces/signatures while preserving item/annotation identity. An ambiguous network failure may have stored content: rerun the unchanged input, not a manually reused signed request.
- This is private authenticated storage in the owner's Cloudflare account, **not end-to-end encryption**. The owner/provider can read EPUBs, source metadata and annotations. Protect local input, cache, downloaded EPUBs, metadata sidecars and exports. File outputs use private temporary files and atomic replacement; stdout can also contain private material. Rotating the dedicated key revokes all clients; removing it disables sync without deleting stored content.

### EPUB push, list, pull, ack and delete

```sh
# First convert/fetch using the normal local workflow, then select a full cache ID.
reader-send inspect --json
reader-send cloud push FULL_24_CHARACTER_CACHE_ID --dry-run
reader-send cloud push FULL_24_CHARACTER_CACHE_ID --source-path /books/article.epub

# Ready entries only; --all also shows unfinished reservations and consumed entries.
reader-send cloud list
reader-send cloud list --all
reader-send cloud pull CLOUD_ITEM_UUID --output-dir ./private-reader-downloads

# After successful installation/reading; pull never acknowledges automatically.
reader-send cloud ack CLOUD_ITEM_UUID
# ACK frees all server EPUB bytes; delete removes the metadata receipt (annotations survive).
reader-send cloud delete CLOUD_ITEM_UUID
```

`--cache-dir` is accepted globally or within cloud commands. `--origin`/`--timeout` can appear after `cloud` or its subcommand; socket timeout defaults to 30 seconds. Cloud push accepts **only an existing cache ID**, not arbitrary source URLs or unconverted documents. It validates the cache hash and EPUB mimetype header before any request, reserves metadata, then uploads the original bytes unchanged.

Item UUIDs deterministically include stable source identity, EPUB SHA-256 and immutable wire metadata, not cache directory, fetched time or read/progress state. Unchanged reruns reuse the UUID; changed bytes/title/source-path produce a new UUID, leaving the prior version until explicitly deleted. `original_url` is preserved exactly from cached source metadata. `source_path` defaults to the stable cached EPUB filename, **not** an absolute computer cache path; use `--source-path` consistently for an intended reader location. The API has no separate author/date/provenance fields: existing local metadata and embedded EPUB metadata are untouched, not squeezed into unrelated fields.

Pull writes `UUID.epub` and `UUID.json` (full server metadata, including original URL/path) in the chosen directory. It checks both listed and returned SHA-256, exact byte size and the EPUB mimetype header **before writing**; server attachment names and source paths never become local filenames. Existing output files are refused unless `--force` is supplied. A filesystem failure can leave one of the two individually atomic outputs; rerun with `--force` after fixing the filesystem. Ready items can be downloaded; after ACK the server EPUB bytes are freed, so a consumed item cannot be downloaded again, though its metadata receipt remains. Unfinished items cannot be downloaded. Ack is idempotent and retains the first consumed timestamp; delete is idempotent and leaves annotations intact.

Cloud limits are **8 MiB per EPUB**, **32 MiB total reserved bytes**, **64 items** including consumed items, and **16 KiB metadata per create request**. Oversized content is rejected locally with a LocalSend/USB recommendation; it is never truncated, stripped or recompressed to fit. Reservations older than 24 hours may expire if unfinished. ACK frees all server EPUB bytes and retains an immutable metadata receipt; deleting the item removes that receipt. Local cached EPUBs and card-delivered books are unaffected.

### Annotation upload, download and full export

```sh
# API-shaped JSON record/array, {"annotations":[...]} wrapper, or .ndjson/.jsonl:
reader-send cloud annotations upload ./private-annotations.json --dry-run
reader-send cloud annotations upload ./private-annotations.json

# Native JSON records copied from /.crosspoint/annotations/ on the reader SD card:
# --device-id is a stable NONSECRET label you choose and reuse for this reader.
reader-send cloud annotations upload ./sd-annotation-copy/ --device-id my-reader --dry-run
reader-send cloud annotations upload ./sd-annotation-copy/ --device-id my-reader

reader-send cloud annotations download --output ./private-annotations.json
reader-send cloud annotations export --output ./private-annotations.ndjson
# Use the printed/stored "after" receive-sequence cursor for the next incremental pull.
reader-send cloud annotations download --after 123 --limit 100 --output ./next-annotations.json
```

All pages are fetched in server **receive-sequence** order, not capture-time order. JSON output includes `after` (last returned sequence, or the input cursor for an empty result); final `next_after` is null. NDJSON export preserves each complete server line and reports the final `after` in the command summary. Do not advance your saved cursor until the output is safely stored. `--force` explicitly replaces existing output; without `--output`, JSON download prints to stdout. There is no annotation deletion API or automatic write-back into the firmware's annotation store.

API-shaped input requires a globally stable lowercase UUID `id`, `book_path`, `source_anchor`, explicit `kind`, and `created_at` in epoch **milliseconds**; `item_id` is optional/null and need not reference an inbox entry. Optional `--item-id UUID` associates an upload with a cloud item, and conflicts with supplied associations are refused. Existing `source_anchor` strings are preserved byte-for-byte in UTF-8, including spacing. A complete `location` object (instead of `source_anchor`), or an object-valued `source_anchor`, is serialized deterministically without dropping any keys. The companion does not substitute display-page numbers for source anchors.

Category mapping is explicit: native `Quote`/0 → `quote`, `Question`/1 → `question`, `ReviewLater`/2 → `review_later` (`AnnotationKind::` prefixes also accepted). All allow a note; the category is never inferred from text. Selection/note whitespace, tabs, newlines and Unicode are preserved.

**Native schema and identity limitation:** firmware records have `sourcePath`, `excerpt`, `note`, `createdAt` (UTC **seconds**, multiplied by 1,000; zero stays unknown/zero), and book-local numeric IDs 1–128 that can be reused. Native uploads therefore require `--device-id` and derive a deterministic UUID from that label, the complete native JSON and optional cloud association. Identical input is idempotent; note/category/location edits create a **new immutable snapshot**, not an update/deletion of the old cloud annotation. Changing the device label/association also creates new identities. Two otherwise identical captures reusing the same slot with unknown time cannot be distinguished by this format; do not claim capture-event identity or revision merging.

Native `source_anchor` contains a `reader-workbench-native-v1` envelope with `device_id` and every native field except `excerpt`/`note` (mapped losslessly to selection/note). It retains original source path/titles, `chapterHref`, `si` (spine), `xpath`, optional `vo` (exact visible offset, **absent differs from zero**), `percentage`, page hints `pc`/`pp`, `pageContext`, version, local ID, original timestamp/category and unknown extra fields. These fields can be reconstructed with selection/note from JSON/NDJSON export; no native import is attempted. Directory input reads `.json` files recursively, ignoring `.tmp`/`.bak`; use the actual annotation store, not Markdown exports.

All input records are validated before upload and split into at most **32 records / 64 KiB raw UTF-8 JSON** per request. Duplicate identical IDs in input are deduplicated; conflicting ones fail before network. Individual limits are 1,024 UTF-16 units for book path, 2,048 for opaque anchor, and 8,192 each for selection/note (matching the JavaScript server's length accounting). A native location too large for the anchor cap is rejected, **never truncated**; retain/export it locally instead. Native/API capture time may not exceed five minutes in the future. Batches commit independently: a later failure reports the number already confirmed, and unchanged reruns safely deduplicate on the server. Edited API-shaped records need new UUIDs; a conflicting UUID causes the entire server batch to roll back.

Server annotation capacity is **4,096 records / 8 MiB canonical JSON**, with no automatic cleanup. Client import/downloads are bounded to 4,096 records / 12 MiB raw JSON (including wrapper/sequence overhead); no partial export is written if pagination, limits or validation fail. All authenticated requests share **60/minute and 1,000/UTC day**, including retries; 429 means wait at least 60 seconds, or until next UTC day for the daily cap. HTTP 401 directs you to the dedicated key/clock, 409 to immutable identity/state, 413 to local delivery, 422 to integrity, 503 to backend setup/availability, and 507 to storage capacity. Errors never echo server bodies, private annotations or credentials.

### One explicit website sync

```sh
# Validate locally and make one read-only remote listing; no writes or uploads.
reader-send cloud sync --dry-run

# One bounded foreground pass: push unpushed cache entries, pull ready books into cache.
reader-send cloud sync

# Direction/limit and annotation controls:
reader-send cloud sync --no-push
reader-send cloud sync --no-pull --max-push 4
reader-send cloud sync --annotations-in "$HOME/papyrix/annotations/reading-inbox-<uuid>.json" --device-id '<uuid>'
reader-send cloud sync --annotations-out "$HOME/papyrix/annotations/cloud-annotations.ndjson"
reader-send cloud sync --incoming-dir ./cloud-incoming

# Finish a large backlog automatically, keeping each batch inside the API budget:
reader-send cloud sync --drain
reader-send cloud sync --drain --annotations-in "$HOME/papyrix/annotations/reading-inbox-<uuid>.json" --device-id '<uuid>' --annotations-out "$HOME/papyrix/annotations/cloud-annotations.ndjson"
```

`cloud sync` opens with one `GET /reader/items` listing, then delegates to the existing verified push/pull and annotation helpers. Each push costs two requests (reserve + upload); each pull costs two (its own audited re-list + content GET). A default run is therefore at most 33 authenticated requests, inside the 60/minute budget. Default caps are 8 pushes and 8 pulls per run; leftovers are reported as deferred. `--drain` keeps the same bounded batch, waits at least 60 seconds between batches so consecutive passes stay inside the 60/minute budget, and repeats until nothing is deferred. It validates the annotation output path before uploading, uploads annotations once before the first batch, and writes the annotation export once after the last; if the upload or a batch stops/raises, the export is still attempted in a `finally` so accepted notes can be inspected. An export failure is reported as `annotations_error` without masking the original batch error. It never retries a failed/ambiguous batch: a network or validation error stops the command. Conflicts, or a batch that leaves both deferred buckets unchanged without moving a new item, stop the loop and report `complete: false`, `stopped`, the conflict details and the remaining deferred count instead of looping. It never deletes or acknowledges items, never starts a background thread, and never runs from fetch/convert/subscriptions/card commands. Pulled books become normal cache entries (unless `--incoming-dir` is chosen), so a following `card sync` copies them. Conflicts and already-synced/already-cached items are reported, not rewritten. If another client acknowledges a listed ready book before this client downloads it, the server answers the content GET with exactly `410 {"error":"content released"}`; that one confirmed response is reported under `pull.released` (item IDs in `cloud sync`, a count in `--drain`), nothing is installed or acknowledged for that book, and the pass continues with the remaining items. Any other 410 (different or unparseable body), a 404, authentication/TLS errors, size/SHA-256/mimetype failures and network errors still stop the command, and `cloud pull <id>` of a consumed item still fails. `--dry-run` still performs that one read-only listing, so it requires `READER_SYNC_KEY` even when annotations are the only requested work; `--dry-run --annotations-out` writes and downloads nothing and returns a planned output.

Shared links from the explicit **Send to Reader** shortcut enter the private `/reader/links` queue. The canonical source is the key-free pair `companion/scripts/send-to-reader.template.wflow` and `companion/scripts/generate_send_to_reader_shortcut.py`; the generator reads the dedicated send-only share key from the Keychain, injects it, and signs the workflow with Apple's `shortcuts sign`. The resulting private installer is `~/papyrix/Reader Workbench/Setup/Send to Reader.shortcut` (mode 0600). To send from the phone, share a URL from the browser's Share Sheet; the Mac launcher supplies the URL input. It POSTs only `{"url": <URL>}` with the scoped share key to `https://example.com/reader/links`, and displays the returned job ID and status under the neutral heading **Reader inbox response**. Never publish or share the signed installer: it contains the send-only secret. The template and generator contain no key.

The currently installed shortcut is the earlier build whose heading was runtime-proven with a valid URL and with invalid/whitespace input. The neutral-heading revision is signed but was not import-tested: replacing the installed copy was blocked while the Mac was locked. iPhone installation requires importing the signed file or receiving iCloud Shortcuts sync; neither was verified on the phone.

The scheduled `collect` drains up to 8 pending links per pass; the manual equivalents are:

```sh
reader-send cloud links list
reader-send cloud links drain --limit 8
```

Each link is converted through the normal public HTTPS fetcher (public Substack/generic articles) or the authorized signed-in Chrome adapter for X/Twitter posts, verified in the local cache, pushed through the existing item flow, and acknowledged only after the item exists. Conversion failures are recorded as explicit terminal job errors; push/ack failures stay pending for the next pass. The queue management key is `READER_SYNC_KEY`; the reader device key and shortcut share key cannot list or acknowledge links. The backend queue must be deployed before these commands can succeed.

A pre-existing Finder **Sync website.command**, if present, is the convenient explicit wrapper. When the exact card is mounted it runs `card sync` **first** (before any cloud secret is read) to refresh `reading-inbox-<uuid>.md` and the managed `reading-inbox-<uuid>.json`. It then reads only the dedicated Keychain item (`security find-generic-password -s reader-workbench:sync-key -a b`) into a child-only `READER_SYNC_KEY`. If that JSON file exists and its `annotations` list is nonempty, it runs `cloud sync --drain --annotations-in <that-file> --device-id <uuid> --annotations-out <annotations-dir>/cloud-annotations.ndjson`; otherwise it runs `cloud sync --drain --annotations-out <annotations-dir>/cloud-annotations.ndjson` only, so a zero-note card never supplies a phantom input. The drain finishes the whole deferred backlog in bounded batches 60 seconds apart; it stops rather than retrying a failed batch and stops on conflicts/no progress with remaining counts in the JSON. It unsets `READER_SYNC_KEY` before the optional final card sync that delivers pulled books. When the card is absent, cloud sync still runs with any retained managed native export, or downloads the annotation export only. The key is never a CLI argument, printed, or written to a file. The bare CLI still requires the environment variable; there is no background cloud sync outside the explicit user-installed `collect` schedule. The current card has zero notes: only the zero-input and annotation-download path is available for verification, and a nonempty native upload has not been live-tested end to end.

## Entirely offline smoke path

From the repository root after installation:

```sh
reader-send convert companion/examples/article.html --cache-dir /tmp/reader-send-sample-cache --output /tmp/reader-send-sample.epub
reader-send inspect --cache-dir /tmp/reader-send-sample-cache --json
reader-send send /tmp/reader-send-sample.epub --reader 192.0.2.1:53317 --dry-run
# Replace the ID with the one printed by inspect; still entirely offline, no key needed.
reader-send cloud push FULL_24_CHARACTER_CACHE_ID --cache-dir /tmp/reader-send-sample-cache --dry-run
```

The sample bundles a small image as a data URL. These commands do not contact the example website or reader. Repeat with a fresh output filename or `--force`.

Focused deterministic tests (no external network, no firmware build):

```sh
python -m pip install -e './companion[browser]'
python -m unittest discover -s companion/tests -v
```

The suite covers article cleanup/metadata/footnotes, hostile paths, ZIP export metadata, bounded images, deterministic EPUB structure, offline CLI/cache flow, robots/retries, RSS/Atom collection and deduplication, source-stable read/progress, X connected-thread/quote/media adapters, and prepare-upload/upload/cancel wire payloads. Collector tests cover environment-first `READER_SYNC_KEY` with a Keychain fallback limited to macOS and never used for a present-but-invalid value, a full card-free `collect` pass with the default `<cache-dir>/annotations/cloud-annotations.ndjson` output, explicit `--annotations-out` use, timeout visibility, and the launchd `ProgramArguments` pinned at install time. Cloud tests use in-memory transports for exact HMAC/body/query identity, verified HTTPS/no redirects/cookies, cache/EPUB integrity, idempotent push/ack/delete and annotation batches, API failures without retries or secret leakage, native field/category mapping, receive-sequence JSON/NDJSON pagination, and no-network/no-secret dry runs. The production API has separate live signed smoke evidence documented above; no multicast delivery, physical reader cloud transfer, or nonempty native-annotation upload from the real card is claimed.
