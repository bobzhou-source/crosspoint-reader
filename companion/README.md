# Reader Workbench companion

`reader-send` turns public Substack/generic web articles or locally saved HTML into EPUB, then optionally sends the EPUB to the reader with LocalSend v2. Conversion and LocalSend run on your computer and require no cloud account, firmware web fetching, browser automation, cookies, or passwords. A separate, explicitly selected `cloud` command can sync already-converted EPUBs and annotations to a private reader API; it is never enabled by fetch/convert/send or subscription collection.

## Install

Python **3.11+**:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install ./companion
reader-send --help
```

On Windows, activate with `.venv\Scripts\Activate.ps1`. If already inside `companion/`, install with `python -m pip install .`. Pillow is the only runtime dependency; parsing, HTTP, caching, EPUB and LocalSend use the standard library. HTTPS still needs a CA trust store: on a python.org macOS installation run its **Install Certificates.command**, or `python -m pip install certifi` and set `SSL_CERT_FILE=$(python -m certifi)` before using `cloud` or `assist`. Never disable certificate or hostname validation.

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

# Manual SD delivery: copy the exported EPUB to your SD card's books folder.
reader-send convert ./saved-post.html --output /Volumes/READER/books/
```

`--title`, `--author`, and `--date` override extracted metadata. `--output` is an EPUB filename or directory; directory/ZIP inputs need a directory. Explicit output files are not overwritten without `--force`. Names are ASCII slugs plus deterministic hash suffixes, at most 90 bytes. `--reader` requires `--send` on fetch/convert, so a target flag alone cannot silently initiate delivery. Multiple discovered receivers require explicit selection.

### Authenticated posts and extraction

Only normal public HTTP GET requests are made. `robots.txt` is checked per origin, including article/image redirects; disallowed or unavailable robots policies stop retrieval. Publisher terms and your authorization still apply. There is no paywall bypass, credential scraping, or JavaScript renderer. A public preview remains a preview; detectable paywalls generate a warning. For subscriber posts, save the **full rendered article you are authorized to access** or supply your own export. Export HTML must actually contain the text; login/app shells cannot be reconstructed.

Metadata comes from Open Graph, HTML headings/bylines/time tags and Article JSON-LD. Extraction prioritizes Substack's article containers and semantic article/main elements, then scores visible prose against link-heavy containers. Scripts, forms, tracking UI, menus, ads, subscription prompts, comments and related-post widgets are removed; headings, lists, tables, quotes, links and image captions are retained. Site-specific layouts can still need local cleanup; inspect the resulting EPUB before delivery.

Local conversion is **offline by default**. Relative images must stay inside the HTML's containing folder, the supplied directory, or the ZIP; embedded image data is supported. Absolute local paths and symlink escapes are rejected. Remote image URLs in saved HTML are omitted with alt text unless `--remote-images` is supplied. With remote article fetching, images are fetched normally; `--no-images` keeps just text/alt text. Image failures warn without losing the prose.

Limits: 8 MiB per HTML/image download, 100,000 HTML nodes / 128 nesting levels, 32 article images, 20 megapixels per decoded image. Images are normalized to metadata-free JPEGs, no longer than 1,200 pixels on either edge and no larger than 512 KiB each; animation uses the first frame. Export ZIPs are limited to 10,000 entries / 256 MiB uncompressed. Directory imports are limited to 1,000 HTML posts. EPUB 2 includes a proper first/uncompressed mimetype, OPF manifest, NCX and well-formed XHTML.

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
reader-send subscriptions list
reader-send subscriptions sync --dry-run
reader-send subscriptions sync
reader-send subscriptions sync --watch --interval 900
reader-send subscriptions remove essays
```

`sync` is also suitable for your own cron/launchd schedule; `--watch` runs until Ctrl-C and does not install a background service. The Mac must remain running. Each pass checks RSS/Atom and imports up to ten new public posts per publication (`--limit 1–100`). It fetches actual article pages rather than mistaking feed excerpts for complete posts. Feed GUIDs and canonical source IDs deduplicate imports; locally supplied full text takes priority over public previews. Changed local HTML regenerates the same source item. An empty/malformed export folder or blocked feed appears in the structured `errors` array; one-shot sync exits 2 on errors, while watch mode reports them and tries again next pass. Overlapping subscription commands are locked.

All sources share `ReadingItem` in `reader_send.model`; `reader_send.extract.Article` remains the same type for existing callers. Metadata includes `source_type`, stable `source_id`, canonical `source_url`, title/author/date, `fetched_at`, warnings, source provenance, `read_state` and fractional `progress`. Source IDs survive content updates; read/progress is stored separately from generated EPUBs and shared by cache variants:

```sh
reader-send inspect --json
reader-send inbox FULL_CACHE_ID --state reading --progress 0.35
reader-send inbox FULL_CACHE_ID --state read
reader-send inbox FULL_CACHE_ID --state unread
```

This is local inbox state, not automatic firmware reading-position synchronization. The opt-in cloud client consumes `Cache.get()` metadata and EPUB bytes without source-specific firmware; it does not change local read/progress state.

## One-action X/Twitter post or thread

```sh
# Offline authorized export → reading inbox (+ optional one-action delivery).
reader-send x 'https://x.com/writer/status/123' --export thread.json
reader-send x 'https://x.com/writer/status/123' --export thread.json --send --reader 192.168.1.42:53317

# Explicit official API access using your existing token in the process environment:
reader-send x 'https://x.com/writer/status/123' --api --output ./thread.epub
```

`--export` accepts official API v2 JSON: `data` is one tweet or an array, with optional `includes.users`, `includes.media` and `includes.tweets` for quoted/parent posts. Posts need `id`, `author_id`, `text` (or `note_tweet.text`), and `referenced_tweets` for connections; include `conversation_id` for stable identity across partial exports, `created_at`, `entities.urls`, and `attachments.media_keys` when available. This is **not** the JavaScript `window.YTD...` account archive format. Export-mode image downloads require `--remote-images`; otherwise conversion remains offline.

The adapter follows connected replies by the original author, orders them by post ID, preserves quoted context, expanded links, authors/dates, accessible pictures and video thumbnails. Other users' replies and disconnected author posts are excluded. Video/Spaces audio is not embedded: an original URL and preview notice remain in the EPUB; metadata contains `media_links` and a `qr_payload` URL for downstream QR rendering (no QR bitmap is generated). Missing/deleted/private posts cannot be reconstructed, and incomplete access is explicitly warned.

`--api` reads `X_BEARER_TOKEN` from the environment without saving it, calls only `api.x.com`, and refuses redirects that could expose the token. It uses one post lookup and at most three 100-result recent-search pages; X's access tier, recent-search window and possible API charges apply. It is opt-in, not a free/public endpoint promise. No automatic API retries, live browser-session extraction or always-on X scraper are provided. On access/quota failure, supply an authorized JSON export instead. Browser accounts/sessions remain Mac-side.

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

LocalSend/USB remain the default. `reader-send cloud …` is the **only** private reader-storage cloud opt-in; there is no background cloud service, automatic upload, URL fetching, conversion, or firmware cloud implementation. The protocol is documented in the portfolio repository's README under **Optional private reader sync**. This implementation and its tests are local only: no deployment, remote migration, installed production secret, live endpoint/WAF test or device cloud test is claimed.

### Setup and security

- An approved deployment owner must first configure the backend and its separate `READER_SYNC_KEY`. The companion only reads that exact environment variable: **64 lowercase hex characters**, representing 32 random bytes. HMAC uses those 64 characters as UTF-8 key bytes, **not decoded hex**. Do not reuse `OWNER_KEY`, `SIGNING_KEY`, browser credentials, or any other token.
- No `--key`/secret CLI argument, secret file, `.env` loading, browser cookie, keychain extraction, or persisted secret is supported. Inject the existing key into the process environment with a secure prompt or your secret manager; do not paste it into shell history. For example, in **Bash** (not zsh), `read -r -s -p 'Reader sync key: ' READER_SYNC_KEY; printf '\n'; export READER_SYNC_KEY` prompts without echo. Use `unset READER_SYNC_KEY` afterward. Dry runs need no key.
- The default explicit origin is `https://robertzhou.dev`. `--origin https://YOUR-HOST[:PORT]` intentionally authorizes that origin to receive both credentials and content; only use a trusted backend you control. HTTP, credentials-in-URL, paths, queries and fragments are refused. TLS certificate and hostname verification are mandatory, with no insecure switch. Standard-library HTTPS does not follow **any** redirects, send/store cookies, or consult proxy environment variables. There is no public-site robots/telemetry request. Requests identify as `ReaderSend/1`; WAF compatibility still requires an approved integration test, not a WAF bypass.
- Each request signs `reader-sync-v1`, uppercase method, exact path/query, SHA-256 of exact body bytes, Unix timestamp and a fresh random 16-byte nonce, LF-separated with no final newline. JSON is serialized once and those same UTF-8 bytes are signed and sent. GET, DELETE and ack have empty bodies.
- System time must be within 300 seconds of the server. **No cloud request is automatically retried.** Rerunning a command creates fresh timestamps/nonces/signatures while preserving item/annotation identity. An ambiguous network failure may have stored content: rerun the unchanged input, not a manually reused signed request.
- This is private authenticated storage in the owner's Cloudflare account, **not end-to-end encryption**. The owner/provider can read EPUBs, source metadata and annotations. Protect local input, cache, downloaded EPUBs, metadata sidecars and exports. File outputs use private temporary files and atomic replacement; stdout can also contain private material. Rotating the dedicated key revokes all clients; removing it disables sync without deleting stored content.

### EPUB push, list, pull, ack and delete

```sh
# First convert/fetch using the normal local workflow, then select a full cache ID.
reader-send inspect --json
reader-send cloud push FULL_24_CHARACTER_CACHE_ID --dry-run
reader-send cloud push FULL_24_CHARACTER_CACHE_ID --source-path /books/article.epub

# Ready entries only; --all also shows unfinished reservations and consumed books.
reader-send cloud list
reader-send cloud list --all
reader-send cloud pull CLOUD_ITEM_UUID --output-dir ./private-reader-downloads

# After successful installation/reading; pull never acknowledges automatically.
reader-send cloud ack CLOUD_ITEM_UUID
# Explicit deletion is the only way to free completed EPUB storage.
reader-send cloud delete CLOUD_ITEM_UUID
```

`--cache-dir` is accepted globally or within cloud commands. `--origin`/`--timeout` can appear after `cloud` or its subcommand; socket timeout defaults to 30 seconds. Cloud push accepts **only an existing cache ID**, not arbitrary source URLs or unconverted documents. It validates the cache hash and EPUB mimetype header before any request, reserves metadata, then uploads the original bytes unchanged.

Item UUIDs deterministically include stable source identity, EPUB SHA-256 and immutable wire metadata, not cache directory, fetched time or read/progress state. Unchanged reruns reuse the UUID; changed bytes/title/source-path produce a new UUID, leaving the prior version until explicitly deleted. `original_url` is preserved exactly from cached source metadata. `source_path` defaults to the stable cached EPUB filename, **not** an absolute computer cache path; use `--source-path` consistently for an intended reader location. The API has no separate author/date/provenance fields: existing local metadata and embedded EPUB metadata are untouched, not squeezed into unrelated fields.

Pull writes `UUID.epub` and `UUID.json` (full server metadata, including original URL/path) in the chosen directory. It checks both listed and returned SHA-256, exact byte size and the EPUB mimetype header **before writing**; server attachment names and source paths never become local filenames. Existing output files are refused unless `--force` is supplied. A filesystem failure can leave one of the two individually atomic outputs; rerun with `--force` after fixing the filesystem. Ready and already-consumed items can be downloaded; unfinished items cannot. Ack is idempotent and retains the first consumed timestamp; delete is idempotent and leaves annotations intact.

Cloud limits are **1 MiB per EPUB**, **32 MiB total reserved bytes**, **64 items** including consumed items, and **16 KiB metadata per create request**. Oversized content is rejected locally with a LocalSend/USB recommendation; it is never truncated, stripped or recompressed to fit. Reservations older than 24 hours may expire if unfinished; completed content persists until deletion.

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
python -m pip install -e ./companion
python -m unittest discover -s companion/tests -v
```

The suite covers article cleanup/metadata/footnotes, hostile paths, ZIP export metadata, bounded images, deterministic EPUB structure, offline CLI/cache flow, robots/retries, RSS/Atom collection and deduplication, source-stable read/progress, X connected-thread/quote/media adapters, and prepare-upload/upload/cancel wire payloads. Cloud tests use in-memory transports for exact HMAC/body/query identity, verified HTTPS/no redirects/cookies, cache/EPUB integrity, idempotent push/ack/delete and annotation batches, API failures without retries or secret leakage, native field/category mapping, receive-sequence JSON/NDJSON pagination, and no-network/no-secret dry runs. No live cloud/API, multicast delivery or physical reader transfer is claimed.
