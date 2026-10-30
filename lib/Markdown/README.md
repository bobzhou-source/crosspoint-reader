# Converted document readers

FB2 and Markdown are streamed into a cached EPUB transport, then read by the native
CrossPoint EPUB activity. This reuses CrossPoint's existing parsed-text layout,
font/orientation settings, page and section caches, chapter navigation, progress,
bookmarks, dictionary, image decoding, and reader menus. Neither converter talks
to the SDK or a network service; storage goes through `HalStorage` / `HalFile`.

## Integration

- Include `Fb2ReaderActivity.h` and `MarkdownReaderActivity.h` in the shared reader
  factory. Construct either with `(renderer, mappedInput, originalPath,
  allowFastInitialRefresh)` using `makeUniqueNoThrow`.
- Dispatch `.fb2` to `Fb2ReaderActivity`; `.md` and `.markdown` to
  `MarkdownReaderActivity`, case-insensitively. File browsing/upload filters must
  recognize the same extensions.
- `EpubReaderActivity` must be non-final with protected virtual
  `std::string prepareBookPath()`, defaulting to `bookPath`. Its load method calls
  the hook, rejects an empty result, and constructs
  `Epub(preparedPath, "/.crosspoint", bookPath)`.
- `Epub`'s optional third constructor argument is the original identity path.
  ZIP reads use the first argument. `getPath()` and cache hashing use the original
  identity, so recents, progress, bookmarks, move-to-read, and sync do not expose
  or act on an internal `converted.epub` file.
- Standalone conversion entry points are `Fb2::prepare(source, cacheRoot)` from
  `<Fb2.h>` and `Markdown::prepare(source, cacheRoot)` from `<Markdown.h>`. Both
  return an empty string on failure, or a complete cached EPUB path on success.
- `ConvertedEpub` in this directory is also available to HTML conversion. It
  streams body XHTML, TOC fragments, image manifest records, and ZIP directory
  records to temporary files rather than holding a book in RAM. Body XHTML is
  `book.xhtml` at ZIP root; TOC anchors omit the leading `#`.
- The activities use existing `STR_INDEXING`; no additional translation keys are
  required. Existing EPUB failure UI handles conversion failures.

## Formats

FB2 uses a two-pass Expat SAX conversion: metadata and embedded PNG/JPEG binaries
first, then formatted body content. The image index is disk-backed. It retains
section/heading anchors, note links and note bodies, paragraphs, headings,
bold/italic/strike/sub/sup, verses, quotations, tables, and supported embedded
images. Metadata comes from `title-info`, not `document-info`. UTF-8/UTF-16 and
Expat's built-in encodings are supported, with explicit Windows-1251, KOI8-R,
and Windows-1252 adapters. DTDs, malformed XML/base64, excessive depth, and parser
allocation exhaustion fail without publishing a partial book. The encoding
adapter retains PapyriX's MIT notice in `lib/Fb2/LICENSE`.

Markdown supports ATX/setext headings, paragraphs and hard/soft line breaks,
bold/italic/bold-italic/strike, escaped punctuation, named/numeric entities,
inline code, fenced/indented code, ordered/unordered lists, blockquotes, pipe
tables, inline links, and local PNG/JPEG images. A bounded YAML frontmatter block
may specify `title`, `author`, and `lang`/`language`; otherwise the first heading
or filename supplies the title. Missing/remote/unsupported images remain readable
as alt text; no remote resource is fetched. Code is rendered with explicit line
breaks and nonbreaking indentation through tags supported by CrossPoint's parser.

This is a constrained Markdown renderer, not a full CommonMark/GFM engine:
reference-style links, embedded raw HTML, nested list layout, and multiline YAML
values are not interpreted. Long physical lines stream in 2 KiB UTF-8-safe
segments without losing text; an inline construct spanning a segment boundary
may remain literal. Metadata is capped at 511 title bytes, 255 author bytes, and
47 language bytes, without cutting a UTF-8 sequence. Inline recursion is bounded
at eight levels. FB2 nesting is capped at 64 and Expat's total live allocations
at 64 KiB. The writer has fixed 1 KiB buffers; document/TOC/image count does not
create an in-memory collection. The ZIP32/NCX entry limits are explicit failures.

## Cache and failure behavior

The transport and native reader state share
`<cacheRoot>/epub_<hash(originalPath)>/`. `converted.meta` contains eight native
little-endian `uint32_t` fields: magic (`DCV1`), converter version, format CRC,
source size, source FNV-1a hash low/high words, archive size, and source modification
time. Each open fingerprints the entire source, detecting same-size edits with
unchanged timestamps. A matching marker and archive size reuse the transport.
The source fingerprint is checked again before publication.

Reconversion publishes only after successful parsing, packaging, and writes.
Native `book.bin`, section, CSS, cover, and thumbnail caches are invalidated;
progress and bookmark files are retained. Temporary conversion files are removed
on both success and failure. The previously published EPUB is not overwritten by
parse failures or early write failures. A failed final rename may leave no
transport; the absent transport prevents a cache hit and the next open rebuilds.
Stored ZIP entries avoid a compression work buffer and trade SD capacity for
bounded RAM. Temporary conversion files require additional SD space.

## Validation

The parent integration pass should run:

```sh
python3 test/document_formats/run.py
./bin/clang-format-fix -g
pio run -e <the selected X4 Pro firmware environment>
```

The host suite compiles the actual converters with filesystem-backed HAL adapters
and system Expat, with ASan/UBSan enabled by default (`SANITIZE=0` disables them).
It validates ZIP CRCs, XML, metadata, heading targets, styles, tables, images,
legacy/UTF-16 FB2 encodings, long UTF-8 lines, truncated caches, same-size source
edits, timestamp-only edits, progress/bookmark preservation, malformed/hostile XML,
and write-failure recovery. HTML cases are included when that module is present.
It does not replace the firmware build or verify physical rendering.

Reader-level acceptance scenarios: open each format, navigate headings and notes,
turn backward/forward across cached pages, bookmark a page, close/reopen and resume,
change font size/orientation and retain position, check end-of-book behavior, and
confirm recents refer to the original source file. These need the parent renderer
or firmware test environment; no physical device operations are part of this slice.
