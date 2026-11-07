"""Convert plain-text Chinese novels into chaptered, fast-opening EPUBs.

The reader builds a book's page index one spine item at a time, so a single
multi-megabyte XHTML file makes the first open slow. This module splits a novel
into chapter files (volume > chapter in the TOC), falls back to size-bounded
files when headings are missing, and refuses to write any book whose extracted
text differs from its normalized source text.
"""
from __future__ import annotations

import codecs
import concurrent.futures
import html
import io
import json
import os
import re
import statistics
import unicodedata
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable
from xml.etree import ElementTree

from .common import ReaderError, atomic_write, bounded_read, digest
from .epub import CONTAINER_XML, pack_epub

CONVERTER_VERSION = 10
UNGROUPED_VERSION = 9  # unchanged books retain their reproducible EPUB identity
TEXT_CLEAN_REVISION = 3  # provenance of the text handling in a build; `convert_tree(rebuild_stale=True)` reconverts older entries
GROUP_CHAPTER_THRESHOLD = 3_000
MANIFEST_NAME = ".novels-manifest.json"
REPORT_NAME = ".novels-report.json"
MANIFEST_VERSION = 1
MAX_MANIFEST_BYTES = 64 * 1024 * 1024
MAX_SOURCE_BYTES = 128 * 1024 * 1024
LANGUAGE = "zh-CN"
# Spine items are cut at paragraph boundaries. A chapter that fits MAX_FILE_BYTES is
# never split; bigger ones are cut near TARGET_FILE_BYTES. The XHTML template adds well
# under 1 KB, so every file stays below CEILING_FILE_BYTES, which the check enforces.
TARGET_FILE_BYTES = 40_000
MAX_FILE_BYTES = 56_000
CEILING_FILE_BYTES = 60_000
MIN_HEADINGS = 5
MAX_HEADING_CHARS = 60
MAX_LABEL_CHARS = 80
MAX_OUTPUT_NAME_BYTES = 200
BARE_SEQUENCE_MINIMUM = 10
BARE_MAX_GAP = 10  # consecutive numbers a bare chain may skip: up to BARE_MAX_GAP - 1 missing headings
BARE_MAX_START = 3  # a bare chain has to begin with one of these numbers
CONTINUATION_TOC_PARTS = 8  # chapters cut into at least this many files also get a TOC entry per file
FRONT_MATTER_LABEL = "卷首"
MODIFIED = "2000-01-01T00:00:00Z"  # fixed so identical input always yields identical bytes
OPF_NS = {"o": "http://www.idpf.org/2007/opf"}
XHTML = "{http://www.w3.org/1999/xhtml}"
NCX = "{http://www.daisy.org/z3986/2005/ncx/}"

STYLE_CSS = (
    "body{line-height:1.5;margin:0}\n"
    "p{text-indent:2em;margin:0}\n"
    "h1,h2,h3{text-indent:0;margin:1em 0}\n"
    "h1{font-size:1.4em}\n"
    "h2,h3{font-size:1.2em}\n"
)


class NovelError(ReaderError):
    """A novel that cannot be converted without risking silent text loss."""


class NovelDecodeError(NovelError):
    """The file is not valid in any encoding this converter accepts."""


class NovelPreservationError(NovelError):
    """The produced EPUB does not contain exactly the source text."""


# --------------------------------------------------------------------------- decoding

# GB18030 files exported by some tools carry UTF-8 punctuation (bullets, CJK marks)
# inside otherwise-GB text. These lead/second-byte pairs are GBK/4 user-area lead
# bytes that real prose never uses, so they are unambiguous enough to split on.
_UTF8_PUNCTUATION = re.compile(rb"\xe2[\x80\x81][\x80-\xbf]|\xe3\x80[\x80-\xbf]")
_XML_INVALID = re.compile("[\x00-\x08\x0b\x0c\x0e-\x1f\ufffe\uffff]")
_INVALID_AS_SPACE = {"\x0b", "\x0c", "\x1c", "\x1d", "\x1e", "\x1f"}
_LINE_BREAKS = str.maketrans({"\u2028": "\n", "\u2029": "\n", "\u0085": "\n"})


_PRIVATE_USE = re.compile("[\ue000-\uf8ff]")


def _decode_mixed(data: bytes) -> tuple[str, int]:
    pieces: list[str] = []
    position = hits = 0
    for match in _UTF8_PUNCTUATION.finditer(data):
        pieces.append(data[position:match.start()].decode("gb18030"))
        pieces.append(match.group().decode("utf-8"))
        position = match.end()
        hits += 1
    if not hits:
        raise UnicodeDecodeError("gb18030", data, 0, 1, "no embedded UTF-8 sequences")
    pieces.append(data[position:].decode("gb18030"))
    return "".join(pieces), hits


def decode_text(data: bytes) -> tuple[str, str, list[str]]:
    """Return (text, encoding, warnings). Never substitutes replacement characters."""
    warnings: list[str] = []
    try:
        if data.startswith(codecs.BOM_UTF8):
            text, encoding = data[len(codecs.BOM_UTF8):].decode("utf-8"), "utf-8-bom"
        elif data.startswith(codecs.BOM_UTF16_LE):
            text, encoding = data[2:].decode("utf-16-le"), "utf-16-le"
        elif data.startswith(codecs.BOM_UTF16_BE):
            text, encoding = data[2:].decode("utf-16-be"), "utf-16-be"
        else:
            try:
                text, encoding = data.decode("utf-8"), "utf-8"
            except UnicodeDecodeError as utf8_error:
                try:
                    text, encoding = data.decode("gb18030"), "gb18030"
                    if _UTF8_PUNCTUATION.search(data) and _PRIVATE_USE.search(text):
                        # Strict GB18030 accepts a UTF-8 bullet followed by ASCII as private-use characters.
                        try:
                            mixed, hits = _decode_mixed(data)
                        except UnicodeDecodeError:
                            pass
                        else:
                            if len(_PRIVATE_USE.findall(mixed)) < len(_PRIVATE_USE.findall(text)):
                                text, encoding = mixed, "gb18030+utf-8"
                                warnings.append(f"Mixed encodings: {hits} UTF-8 punctuation sequences inside GB18030 text")
                except UnicodeDecodeError as gb_error:
                    try:
                        text, hits = _decode_mixed(data)
                    except UnicodeDecodeError:
                        raise NovelDecodeError(
                            f"Undecodable text: not UTF-8 (byte {utf8_error.start}: {utf8_error.reason}) "
                            f"and not GB18030 (byte {gb_error.start}: {gb_error.reason}); "
                            "convert the file to UTF-8 first") from None
                    encoding = "gb18030+utf-8"
                    warnings.append(f"Mixed encodings: {hits} UTF-8 punctuation sequences inside GB18030 text")
    except UnicodeDecodeError as exc:
        raise NovelDecodeError(f"Undecodable text: byte {exc.start}: {exc.reason}") from None
    nuls = text.count("\x00")
    if nuls * 1000 > len(text):
        # Isolated NULs are extraction debris (removed and counted by normalize_text); this many is not text.
        raise NovelDecodeError(f"Undecodable text: {nuls} NUL characters in {len(text)} (binary data or UTF-16/32 without a byte-order mark)")
    return text, encoding, warnings


def normalize_text(text: str) -> tuple[str, int]:
    """Normalize newlines and drop characters XML cannot carry; return (text, dropped)."""
    text = text.replace("\r\n", "\n").replace("\r", "\n").translate(_LINE_BREAKS)
    dropped = 0
    if _XML_INVALID.search(text):
        table = {}
        for code in list(range(0x20)) + [0xFFFE, 0xFFFF]:
            char = chr(code)
            if _XML_INVALID.match(char) and char != "\n":
                table[code] = " " if char in _INVALID_AS_SPACE else None
        dropped = sum(text.count(chr(code)) for code, value in table.items() if value is None)
        text = text.translate(table)
    return text, dropped


_DECORATION_LINE = re.compile(r"[\s#=\-*☆★※]+")
_LEADING_DECORATION = re.compile(r"(?:[\s#=\-*☆★※]*[☆★]、)?[\s#=\-*☆★※]*")
_TRAILING_DECORATION = re.compile(r"[\s#=\-*☆★※]+$")
_TRAILING_REMOVABLE = frozenset("#=-*☆★※")
_LEADING_REMOVABLE = _TRAILING_REMOVABLE | {"、"}  # the comma of a Jinjiang `☆、` marker goes with the star


def _non_blank_lines(text: str) -> list[str]:
    """One entry per non-blank line, trimmed of whitespace (incl. U+3000 and tabs)."""
    return [line for line in (raw.strip() for raw in text.split("\n")) if line]


def paragraphs_of(text: str) -> list[str]:
    """One paragraph per non-blank, non-decoration line; surrounding whitespace removed."""
    return [line for line in _non_blank_lines(text) if not _DECORATION_LINE.fullmatch(line)]


def read_structure(text: str) -> tuple[list[str], dict[int, Heading], list[str]]:
    """(paragraphs, headings by paragraph index, dropped decoration lines) of normalized text.

    Lines made only of decoration characters (`====`, `----`, `****`, `☆☆☆`, `* * *`) are not paragraphs; they
    are returned separately so the preservation check can account for them. Headings are detected before
    they are dropped, so a rule line under a title still separates it from the next heading exactly as it
    does in the source, and no heading can be a decoration line.
    """
    lines = _non_blank_lines(text)
    found = detect_headings(lines)
    paragraphs: list[str] = []
    headings: dict[int, Heading] = {}
    ornaments: list[str] = []
    for index, line in enumerate(lines):
        if _DECORATION_LINE.fullmatch(line):
            ornaments.append(line)
            continue
        if index in found:
            headings[len(paragraphs)] = found[index]
        paragraphs.append(line)
    return paragraphs, headings, ornaments


def strip_decoration(text: str) -> str:
    """A heading without leading/trailing `# = - * ☆ ★ ※` and spaces (and the `、` of a `☆、` marker)."""
    stripped = _TRAILING_DECORATION.sub("", _LEADING_DECORATION.sub("", text, count=1))
    return stripped or text


# ---------------------------------------------------------------------------- naming

_BRACKET_AUTHOR = re.compile(r"^\s*[\[［【]\s*([^\]］】]+?)\s*[\]］】]\s*(\S.*?)\s*$")
_QUOTED_TITLE = re.compile(r"^\s*《\s*([^》]+?)\s*》\s*(?:作者\s*[:：]\s*(\S.*?)|(\S.*?)\s*著)?\s*$")
_AUTHOR_SUFFIX = re.compile(r"^\s*(\S.*?)\s*[（(]?\s*作者\s*[:：]\s*(\S.*?)\s*[）)]?\s*$")
_UNDERSCORE = re.compile(r"^\s*([^_]+?)\s*_\s*([^_]+?)\s*$")
_IN_FILE_AUTHOR = re.compile(r"^作者\s*[:：]\s*(\S.{0,40})$")


def _unquote_title(title: str) -> str:
    match = re.fullmatch(r"\s*《\s*([^》]+?)\s*》\s*", title)
    return match.group(1) if match else title.strip()


def parse_filename(stem: str) -> tuple[str, str]:
    """Title and author from `[作者]书名`, `《书名》作者：X`, `书名_作者` and similar stems."""
    stem = unicodedata.normalize("NFC", stem).strip()
    match = _BRACKET_AUTHOR.match(stem)
    if match:
        return _unquote_title(match.group(2)), match.group(1).strip()
    match = _QUOTED_TITLE.match(stem)
    if match:
        return match.group(1).strip(), (match.group(2) or match.group(3) or "").strip()
    match = _AUTHOR_SUFFIX.match(stem)
    if match:
        return _unquote_title(match.group(1)), match.group(2).strip()
    match = _UNDERSCORE.match(stem)
    if match:
        return _unquote_title(match.group(1)), match.group(2).strip()
    return _unquote_title(stem) or "Untitled", ""


_FAT_UNSAFE = str.maketrans({"\\": "＼", "/": "／", ":": "：", "*": "＊", "?": "？", '"': "＂",
                             "<": "＜", ">": "＞", "|": "｜"})


def output_name(stem: str) -> str:
    """A card-safe EPUB basename: FAT-legal, NFC, and short enough for the reader's library index."""
    name = unicodedata.normalize("NFC", stem).translate(_FAT_UNSAFE)
    name = "".join(c for c in name if ord(c) >= 0x20 and ord(c) != 0x7F).strip().rstrip(". ")
    if not name:
        name = "novel"
    if len(name.encode("utf-8")) + len(".epub") > MAX_OUTPUT_NAME_BYTES:
        keep = name.encode("utf-8")[:MAX_OUTPUT_NAME_BYTES - 20].decode("utf-8", errors="ignore")
        name = f"{keep}-{digest(stem)[:8]}"
    return name + ".epub"


# ------------------------------------------------------------------ heading detection

_NUMERAL = r"(?:[0-9０-９]+|[零〇一二三四五六七八九十百千万萬两兩壹贰貳叁參肆伍陆陸柒捌玖拾佰仟]+)"
_LEAD = r"[\s\u3000#＃*＊=＝\-_—·•【\[（(《〈「『☆★◆◇●○■□▲△※、]*"
_NUMBERED = re.compile(rf"^{_LEAD}第\s*({_NUMERAL})\s*([章节節回卷集部篇话話])")
_ECHO_KEY = re.compile(rf"^[^第]{{0,12}}第\s*({_NUMERAL})\s*([章节節回卷集部篇话話])")  # also `## 正文_第一章`
_SPECIAL = re.compile(
    rf"^{_LEAD}(序章|序言|楔子|引子|前言|尾声|尾聲|终章|終章|番外|后记|後記|完本感言|完结感言|完結感言|Prologue|Epilogue)"
    r"(?=$|[\s\u3000：:、，,.．…—\-·（(\[【\d一二三四五六七八九十上中下篇])", re.IGNORECASE)
_ENGLISH = re.compile(
    rf"^{_LEAD}(?:\d+\s*[、.．]\s*)?chapter\s*(?:\d+|[ivxlcdm]+|one|two|three|four|five|six|seven|eight|nine|ten)\b",
    re.IGNORECASE)
_WRAPPED = re.compile(r"^#{3}\s*([^#\s].*?)\s*#{3}$")
_STAR = re.compile(r"^[☆★]、\S")
_HASHED = re.compile(r"^(#{1,3})\s+(\S.*)$")
_HASHED_CHAPTER = re.compile(r"^#{2,3}\s+\S")
_BARE = re.compile(rf"^({_NUMERAL})(?:\s*[、.．,，:：]\s*|\s+)(\S.*)$")
_BARE_GLUED = re.compile(r"^([0-9０-９]+)(?=[\u4e00-\u9fff])(\S.*)$")  # `836克苏鲁…`: some sources drop the separator
_LABEL_TRIM = re.compile(r"^[\s\u3000#＃*＊=＝☆★、]+|[\s\u3000#＃*＊=＝]+$")
_FULLWIDTH_DIGITS = str.maketrans("０１２３４５６７８９", "0123456789")
_DIGITS = {"零": 0, "〇": 0, "一": 1, "二": 2, "两": 2, "兩": 2, "三": 3, "四": 4, "五": 5, "六": 6, "七": 7,
           "八": 8, "九": 9, "壹": 1, "贰": 2, "貳": 2, "叁": 3, "參": 3, "肆": 4, "伍": 5, "陆": 6, "陸": 6,
           "柒": 7, "捌": 8, "玖": 9}
_UNITS = {"十": 10, "拾": 10, "百": 100, "佰": 100, "千": 1000, "仟": 1000}
VOLUME_MARKERS = "卷部集"

VOLUME, CHAPTER, ECHO = 1, 2, 3


@dataclass(frozen=True)
class Heading:
    level: int  # 1 volume, 2 chapter, 3 echo: repeats the number of the heading before it and joins its chapter
    label: str


def parse_numeral(text: str) -> int | None:
    """Value of an Arabic (ASCII or fullwidth) or Chinese numeral, or None."""
    text = text.translate(_FULLWIDTH_DIGITS)
    if re.fullmatch(r"[0-9]+", text):
        return int(text)
    if text and all(c in _DIGITS for c in text) and len(text) > 1 and not any(c in _UNITS for c in text):
        return int("".join(str(_DIGITS[c]) for c in text))  # positional: 二〇二三
    total = section = number = 0
    for char in text:
        if char in _DIGITS:
            number = _DIGITS[char]
        elif char in _UNITS:
            section += (number or 1) * _UNITS[char]
            number = 0
        elif char in "万萬":
            total += ((section + number) or 1) * 10000
            section = number = 0
        else:
            return None
    return total + section + number


def _plausible(line: str) -> bool:
    return (len(line) <= MAX_HEADING_CHARS and "。" not in line
            and not line.endswith(("，", "、", "；", "：", ",", ";")))


def _label(line: str) -> str:
    label = _LABEL_TRIM.sub("", line)
    return label[:MAX_LABEL_CHARS] if label else line[:MAX_LABEL_CHARS]


def classify_heading(line: str, *, hash_headings: bool = False) -> Heading | None:
    """Classify one stripped paragraph as a volume/chapter heading, or None.

    `hash_headings` enables Markdown `#`/`##` headings, which only count in files that
    clearly use them as chapter markers (three or more `##` lines).
    """
    if len(line) > MAX_HEADING_CHARS:
        return None
    match = _NUMBERED.match(line)
    if match and _plausible(line):
        return Heading(VOLUME if match.group(2) in VOLUME_MARKERS else CHAPTER, _label(line))
    if _plausible(line) and (_SPECIAL.match(line) or _ENGLISH.match(line)):
        return Heading(CHAPTER, _label(line))
    if line[:1] in "☆★" and _STAR.match(line) and _plausible(line):  # Jinjiang style "☆、标题"
        return Heading(CHAPTER, _label(line))
    if line.startswith("#") and _plausible(line):
        if _WRAPPED.match(line):
            return Heading(CHAPTER, _label(line))
        if hash_headings:
            hashes = _HASHED.match(line)
            if hashes:
                return Heading(VOLUME if len(hashes.group(1)) == 1 else CHAPTER, _label(line))
    return None


def _chapter_key(line: str) -> tuple[str, int] | None:
    """(unit, number) of a `第N章`-style line; zero padding and Chinese/Arabic/fullwidth numerals normalize away."""
    match = _ECHO_KEY.match(line)
    number = parse_numeral(match.group(1)) if match else None
    return (match.group(2), number) if number is not None else None


def _echo_identity(text: str) -> tuple[tuple[str, int] | None, str] | None:
    """What a heading shows, for comparing a title with its echoes.

    The chapter number is compared as a number — `第200章`, `第00200章` and `第二百章` are the same
    chapter — and the remaining characters without any whitespace. Sources reprint a title with a
    different numeral form or different spacing, which the reader cannot show as anything but the
    same line. Everything else is compared as written.
    """
    stripped = strip_decoration(text)
    match = _ECHO_KEY.match(stripped)
    number = parse_numeral(match.group(1)) if match else None
    if match and number is not None:
        return (match.group(2), number), "".join(stripped[match.end():].split())
    visible = "".join(stripped.split())
    return (None, visible) if visible else None


def _echoes(paragraphs: list[str], found: dict[int, Heading]) -> dict[int, int]:
    """{echo index: index of the heading it repeats} for titles that only repeat the one before.

    `## 第二章 标题` / `====` / `第002章`: only blank or decoration-only lines may lie between the two, so the
    echo has no body of its own and belongs to the chapter of the heading it repeats instead of opening one.
    The repeat may print the number in another numeral form or with different spacing (see `_echo_identity`).
    """
    echoes: dict[int, int] = {}
    anchor: tuple[int, tuple[tuple[str, int] | None, str] | None, tuple[str, int] | None] | None = None
    for index in sorted(found):
        key = _chapter_key(paragraphs[index])
        identity = _echo_identity(paragraphs[index])
        repeated = (anchor is not None and identity is not None
                    and (identity == anchor[1] or (key is not None and key == anchor[2]))
                    and all(i in echoes or _DECORATION_LINE.fullmatch(paragraphs[i])
                            for i in range(anchor[0] + 1, index)))
        if repeated:
            echoes[index] = anchor[0]
        else:
            anchor = (index, identity, key)
    return echoes


def detect_headings(paragraphs: list[str]) -> dict[int, Heading]:
    """Heading candidates by paragraph index, after demoting headings that have no body.

    A heading that repeats the number of the one before it (see `_echoes`) is returned with level ECHO.
    """
    hash_headings = sum(1 for p in paragraphs if p[:1] == "#" and _HASHED_CHAPTER.match(p)) >= 3
    found: dict[int, Heading] = {}
    for index, paragraph in enumerate(paragraphs):
        heading = classify_heading(paragraph, hash_headings=hash_headings)
        if heading:
            found[index] = heading
    if len(found) < MIN_HEADINGS:
        found = _bare_sequence(paragraphs, found)
    if not found:
        return {}
    volumes = sum(1 for h in found.values() if h.level == VOLUME)
    if volumes == 0 or volumes > len(found) - volumes:
        found = {i: Heading(CHAPTER, h.label) for i, h in found.items()}
    owners = _echoes(paragraphs, found)
    echoes = {i: h for i, h in found.items() if i in owners}
    found = {i: h for i, h in found.items() if i not in owners}
    last_line = {owner: echo for echo, owner in owners.items()}  # ascending, so the final echo wins
    # A heading directly followed by another heading has no body: it is a table-of-contents
    # line or a stray title, so it stays plain text rather than becoming an empty chapter.
    # Its echoes are no body either, and go back to plain text with it.
    kept: dict[int, Heading] = {}
    for index, heading in found.items():
        end = last_line.get(index, index)
        following = found.get(end + 1)
        if end + 1 >= len(paragraphs):
            continue
        if following is not None and not (heading.level == VOLUME and following.level == CHAPTER):
            continue
        kept[index] = heading
    kept.update((i, Heading(ECHO, h.label)) for i, h in echoes.items() if owners[i] in kept)
    return dict(sorted(kept.items()))


def _bare_sequence(paragraphs: list[str], existing: dict[int, Heading]) -> dict[int, Heading]:
    """Headings such as `一 标题` / `077，标题` that count 1, 2, 3, ... through the whole file.

    The longest chain that starts near 1 and never skips BARE_MAX_GAP or more numbers wins, so a missing
    run of chapters or a stray `5.5倍` line in the text neither breaks nor joins the sequence. When two
    lines carry the same number the later one wins, which prefers a chapter over a table-of-contents line
    at the top. A chain whose lines sit next to other numbered lines is a list (forum floors, outlines),
    not a run of chapters, and is refused.
    """
    best: dict[int, tuple] = {}  # value -> (length, paragraph index, previous node, has a separator)
    candidates: set[int] = set()
    for index, paragraph in enumerate(paragraphs):
        if len(paragraph) > 40 or paragraph[:1] in "#":
            continue
        match = _BARE.match(paragraph) or _BARE_GLUED.match(paragraph)
        if not match or not _plausible(paragraph):
            continue
        value = parse_numeral(match.group(1))
        if value is None or value < 1:
            continue
        candidates.add(index)
        previous = max((best[v] for v in range(value - BARE_MAX_GAP, value) if v in best), key=lambda node: node[0], default=None)
        if previous is None and value > BARE_MAX_START:
            continue
        node = ((previous[0] if previous else 0) + 1, index, previous, int(bool(_BARE.match(paragraph))))
        if value not in best or (node[0], node[3]) >= (best[value][0], best[value][3]):
            best[value] = node
    if not best:
        return existing
    tail = max(best.values(), key=lambda node: node[0])
    chain: dict[int, Heading] = {}
    while tail is not None:
        chain[tail[1]] = Heading(CHAPTER, _label(paragraphs[tail[1]]))
        tail = tail[2]
    crowded = sum(1 for i in chain if i - 1 in candidates or i + 1 in candidates)
    if len(chain) < BARE_SEQUENCE_MINIMUM or crowded * 4 > len(chain):
        return existing
    return dict(sorted(chain.items()))


# ---------------------------------------------------------------------- segmentation

@dataclass
class Segment:
    level: int  # 0 front matter / unstructured text, 1 volume, 2 chapter
    label: str
    heading: str | None
    body: list[str]
    echoes: list[str] = field(default_factory=list)  # lines repeating the heading's number, shown right under it


def chapter_starts(headings: dict[int, Heading]) -> list[int]:
    """Paragraph indices that open a volume or chapter (an echo belongs to the heading before it)."""
    return sorted(i for i, h in headings.items() if h.level != ECHO)


def duplicate_echoes(paragraphs: list[str], headings: dict[int, Heading]) -> set[int]:
    """Paragraph indices of echo lines the reader would show as a second copy of the title.

    `第1章 标题` / `第1章 标题` shows the same title twice in a row; the second line is dropped (and
    accounted for as removed text). The comparison is `_echo_identity` — whitespace aside, and with
    the chapter number read as a number — so `第二百章 标题` after `第200章 标题` is the same title.
    An echo that repeats an earlier echo of the same chapter is dropped too, and an echo that adds
    any visible character is kept under the heading.
    """
    if len(chapter_starts(headings)) < MIN_HEADINGS:
        return set()
    dropped: set[int] = set()
    owner: int | None = None
    shown: set = set()
    for index in sorted(headings):
        if headings[index].level != ECHO:
            owner = index
            identity = _echo_identity(paragraphs[index])
            shown = {identity} if identity is not None else set()
        elif owner is not None:
            identity = _echo_identity(paragraphs[index])
            if identity is None:
                continue
            if identity in shown:
                dropped.add(index)
            else:
                shown.add(identity)
    return dropped


def segment(paragraphs: list[str], headings: dict[int, Heading]) -> list[Segment]:
    starts = chapter_starts(headings)
    if len(starts) < MIN_HEADINGS:
        return [Segment(0, "", None, paragraphs)]
    duplicates = duplicate_echoes(paragraphs, headings)
    segments: list[Segment] = []
    if starts[0] > 0:
        segments.append(Segment(0, FRONT_MATTER_LABEL, None, paragraphs[:starts[0]]))
    for position, start in enumerate(starts):
        end = starts[position + 1] if position + 1 < len(starts) else len(paragraphs)
        heading = headings[start]
        first = start + 1
        while first < end and first in headings:  # the only headings inside a segment are its echoes
            first += 1
        echoes = [strip_decoration(paragraphs[i]) for i in range(start + 1, first) if i not in duplicates]
        segments.append(Segment(heading.level, heading.label, strip_decoration(paragraphs[start]),
                                paragraphs[first:end], echoes))
    return segments


@dataclass
class Block:
    tag: str
    markup: str  # escaped element body
    cost: int
    anchor: str = ""


def _escape(text: str) -> str:
    return html.escape(text, quote=False)


def _block(tag: str, text: str) -> Block:
    escaped = _escape(text)
    return Block(tag, escaped, len(escaped.encode("utf-8")) + 2 * len(tag) + 5)


_SENTENCE_END = re.compile(r"(?<=[。！？!?…；;])")


def _split_long(text: str, budget: int) -> list[str]:
    """Cut one oversize paragraph at sentence ends (hard cut only for punctuation-free runs)."""
    pieces: list[str] = []
    current: list[str] = []
    used = 0
    for sentence in _SENTENCE_END.split(text):
        if not sentence:
            continue
        cost = len(_escape(sentence).encode("utf-8"))
        if cost > budget:
            if current:
                pieces.append("".join(current))
                current, used = [], 0
            step = max(1, budget // 5)
            pieces.extend(sentence[i:i + step] for i in range(0, len(sentence), step))
            continue
        if current and used + cost > budget:
            pieces.append("".join(current))
            current, used = [], 0
        current.append(sentence)
        used += cost
    if current:
        pieces.append("".join(current))
    return pieces


def _body_blocks(paragraphs: list[str], splits: list[int]) -> list[Block]:
    blocks: list[Block] = []
    for paragraph in paragraphs:
        block = _block("p", paragraph)
        if block.cost <= MAX_FILE_BYTES:
            blocks.append(block)
            continue
        splits[0] += 1
        blocks.extend(_block("p", piece) for piece in _split_long(paragraph, MAX_FILE_BYTES - 64))
    return blocks


def group_blocks(blocks: list[Block]) -> list[list[Block]]:
    """Group blocks into files: whole if they fit, else cut near TARGET at block boundaries."""
    total = sum(b.cost for b in blocks)
    if total <= MAX_FILE_BYTES:
        return [blocks]
    groups: list[list[Block]] = []
    current: list[Block] = []
    size = 0
    for block in blocks:
        if current and size + block.cost > MAX_FILE_BYTES:
            groups.append(current)
            current, size = [], 0
        current.append(block)
        size += block.cost
        if size >= TARGET_FILE_BYTES:
            groups.append(current)
            current, size = [], 0
    if current:
        tail = sum(b.cost for b in current)
        previous = sum(b.cost for b in groups[-1]) if groups else 0
        if groups and tail < TARGET_FILE_BYTES // 4 and previous + tail <= MAX_FILE_BYTES:
            groups[-1].extend(current)
        else:
            groups.append(current)
    return groups


@dataclass
class Page:
    title: str
    blocks: list[Block]
    toc: list[tuple[int, str, str]] = field(default_factory=list)  # (depth, label, fragment)


@dataclass
class Plan:
    pages: list[Page]
    mode: str  # "headings" or "size"
    volumes: int = 0
    chapters: int = 0
    split_segments: int = 0
    paragraph_splits: int = 0


def plan_pages(segments: list[Segment]) -> Plan:
    splits = [0]
    pages: list[Page] = []
    size_mode = len(segments) == 1 and segments[0].level == 0 and segments[0].heading is None and segments[0].label == ""
    plan = Plan(pages, "size" if size_mode else "headings")
    seen_volume = False
    for seg in segments:
        blocks: list[Block] = []
        if seg.heading is not None:
            blocks.append(_block("h1" if seg.level == VOLUME else "h2", seg.heading))
            blocks.extend(_block("h3", echo) for echo in seg.echoes)
        blocks.extend(_body_blocks(seg.body, splits))
        groups = group_blocks(blocks) if blocks else [[]]
        if len(groups) > 1 and not size_mode:
            plan.split_segments += 1
        if seg.level == VOLUME:
            plan.volumes += 1
            depth, seen_volume = 1, True
        elif seg.level == CHAPTER:
            plan.chapters += 1
            depth = 2 if seen_volume else 1
        else:
            depth = 1
        for number, group in enumerate(groups, 1):
            if size_mode:
                label = f"第{number}部分"
                pages.append(Page(label, group, [(1, label, "")]))
            else:
                if number == 1:
                    toc = [(depth, seg.label, "")]
                elif len(groups) >= CONTINUATION_TOC_PARTS:
                    toc = [(depth, f"{seg.label}（{number}）", "")]  # a very long chapter stays navigable
                else:
                    toc = []
                pages.append(Page(seg.label, group, toc))
    plan.paragraph_splits = splits[0]
    if plan.chapters > GROUP_CHAPTER_THRESHOLD:
        packed: list[Page] = []
        used = 0
        for index, page in enumerate(pages, 1):
            if page.toc:
                anchor = f"c{index:05d}"
                page.blocks[0].anchor = anchor
                page.blocks[0].cost += len(anchor) + 6  # ' id="..."'
                page.toc = [(depth, label, anchor) for depth, label, _ in page.toc]
            size = sum(block.cost for block in page.blocks)
            if packed and used + size <= MAX_FILE_BYTES:
                packed[-1].blocks.extend(page.blocks)
                packed[-1].toc.extend(page.toc)
                used += size
            else:
                packed.append(page)
                used = size
        plan.pages = packed
    return plan


# --------------------------------------------------------------------------- EPUB

def _xhtml(title: str, blocks: list[Block]) -> str:
    body = "\n".join(f'<{b.tag}' + (f' id="{b.anchor}"' if b.anchor else "") + f'>{b.markup}</{b.tag}>'
                     for b in blocks)
    return (f'<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{LANGUAGE}" lang="{LANGUAGE}"><head>'
            f'<title>{_escape(title)}</title>\n<link rel="stylesheet" type="text/css" href="style.css" />'
            f'</head><body>\n{body}\n</body></html>')


def _page_name(index: int) -> str:
    return f"s{index + 1:05d}.xhtml"


def _toc_tree(entries: list[tuple[int, str, str]]) -> list[dict]:
    root: list[dict] = []
    stack: list[tuple[int, dict]] = []
    for depth, label, href in entries:
        node = {"label": label, "href": href, "children": []}
        while stack and stack[-1][0] >= depth:
            stack.pop()
        (stack[-1][1]["children"] if stack else root).append(node)
        stack.append((depth, node))
    return root


def _nav_list(nodes: list[dict]) -> str:
    items = []
    for node in nodes:
        children = f"\n{_nav_list(node['children'])}" if node["children"] else ""
        items.append(f'<li><a href="{_escape(node["href"])}">{_escape(node["label"])}</a>{children}</li>')
    return "<ol>\n" + "\n".join(items) + "\n</ol>"


def _ncx_points(nodes: list[dict], counter: list[int]) -> str:
    points = []
    for node in nodes:
        counter[0] += 1
        number = counter[0]
        inner = _ncx_points(node["children"], counter) if node["children"] else ""
        points.append(f'<navPoint id="n{number}" playOrder="{number}"><navLabel><text>{_escape(node["label"])}</text>'
                      f'</navLabel><content src="{_escape(node["href"])}"/>{inner}</navPoint>')
    return "\n".join(points)


def build_novel_epub(title: str, author: str, plan: Plan, identity: str) -> bytes:
    version = CONVERTER_VERSION if plan.chapters > GROUP_CHAPTER_THRESHOLD else UNGROUPED_VERSION
    uid = "urn:sha256:" + digest(f"novel:{version}:{identity}")
    entries: dict[str, bytes | str] = {"mimetype": "application/epub+zip", "META-INF/container.xml": CONTAINER_XML}
    toc: list[tuple[int, str, str]] = []
    manifest = []
    spine = []
    for index, page in enumerate(plan.pages):
        name = _page_name(index)
        manifest.append(f'<item id="s{index + 1:05d}" href="{name}" media-type="application/xhtml+xml"/>')
        spine.append(f'<itemref idref="s{index + 1:05d}"/>')
        toc.extend((depth, label, f"{name}#{anchor}" if anchor else name) for depth, label, anchor in page.toc)
    tree = _toc_tree(toc)
    depth = 2 if any(node["children"] for node in tree) else 1
    creator = f"<dc:creator>{_escape(author)}</dc:creator>" if author else ""
    esc_title = _escape(title)
    entries["OEBPS/content.opf"] = (
        f'<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="book-id">\n'
        f'<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="book-id">{uid}</dc:identifier>\n'
        f'<dc:title>{esc_title}</dc:title><dc:language>{LANGUAGE}</dc:language>{creator}\n'
        f'<meta property="dcterms:modified">{MODIFIED}</meta></metadata>\n'
        f'<manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>\n'
        f'<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>\n'
        f'<item id="style" href="style.css" media-type="text/css"/>\n' + "\n".join(manifest) + "\n</manifest>\n"
        f'<spine toc="ncx">\n' + "\n".join(spine) + "\n</spine></package>")
    entries["OEBPS/nav.xhtml"] = (
        f'<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" '
        f'xml:lang="{LANGUAGE}" lang="{LANGUAGE}"><head><title>{esc_title}</title></head><body>\n'
        f'<nav epub:type="toc" id="toc">\n{_nav_list(tree)}\n</nav></body></html>')
    entries["OEBPS/toc.ncx"] = (
        f'<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head><meta name="dtb:uid" content="{uid}"/>\n'
        f'<meta name="dtb:depth" content="{depth}"/><meta name="dtb:totalPageCount" content="0"/>'
        f'<meta name="dtb:maxPageNumber" content="0"/></head>\n'
        f'<docTitle><text>{esc_title}</text></docTitle><navMap>\n{_ncx_points(tree, [0])}\n</navMap></ncx>')
    entries["OEBPS/style.css"] = STYLE_CSS
    # Keep startup lookups ahead of thousands of chapter entries: the reader
    # locates ZIP members by scanning the central directory from its beginning.
    for index, page in enumerate(plan.pages):
        entries[f"OEBPS/{_page_name(index)}"] = _xhtml(page.title, page.blocks)
    return pack_epub(entries)


# --------------------------------------------------------------------- verification

def apply_decoration(paragraphs: list[str], headings: dict[int, Heading]) -> list[str]:
    """Paragraph texts as the EPUB shows them: structural headings lose their edge decoration."""
    if len(chapter_starts(headings)) < MIN_HEADINGS:  # size mode has no headings
        return paragraphs
    return [strip_decoration(p) if i in headings else p for i, p in enumerate(paragraphs)]


def _edge_removal(original: str, shown: str) -> int:
    """Non-whitespace characters `shown` lacks; raise unless they are all decoration at the edges of `original`."""
    before = "".join(original.split())
    after = "".join(shown.split())
    start = before.find(after)
    if start < 0 or not set(before[:start]) <= _LEADING_REMOVABLE or not set(before[start + len(after):]) <= _TRAILING_REMOVABLE:
        raise NovelPreservationError(f"Heading {original!r} lost more than edge decoration (shown as {shown!r})")
    return len(before) - len(after)


def expected_book_text(normalized: str) -> tuple[str, dict]:
    """The source text the EPUB has to hold, without whitespace, plus what the decoration rule removed.

    The only edits to the source are the documented ones: leading/trailing `# = - * ☆ ★ ※` (and the
    `、` of a `☆、` marker) on heading lines, and lines made of nothing but those characters. Every
    removal is verified to sit at the edge of its heading or to be a whole decoration line, and the
    total is reconciled with the length of the source, so nothing else can disappear unnoticed. The one
    other removal is an echo line that repeats its heading's visible characters (`duplicate_echoes`),
    counted whole in `duplicate_heading_chars_removed`, never in `removed_decoration_chars`.
    """
    paragraphs, headings, ornaments = read_structure(normalized)
    shown = apply_decoration(paragraphs, headings)
    duplicates = duplicate_echoes(paragraphs, headings)
    removed = sum(len("".join(line.split())) for line in ornaments)
    duplicate_chars = 0
    cleaned = 0
    for index, (original, text) in enumerate(zip(paragraphs, shown)):
        if index in duplicates:
            duplicate_chars += len("".join(original.split()))
            shown[index] = ""
        elif original != text:
            removed += _edge_removal(original, text)
            cleaned += 1
    expected = "".join("".join(shown).split())
    if len("".join(normalized.split())) != len(expected) + removed + duplicate_chars:
        raise NovelPreservationError("Decoration accounting does not add up to the source text")
    return expected, {"removed_decoration_chars": removed, "decoration_lines_dropped": len(ornaments),
                      "headings_cleaned": cleaned, "duplicate_headings_dropped": len(duplicates),
                      "duplicate_heading_chars_removed": duplicate_chars}


def check_preservation(epub: bytes, normalized: str) -> dict:
    """Independently re-read the EPUB; raise unless it holds exactly the expected source text.

    The expected text is the normalized source with only the documented decoration removal applied
    (see `expected_book_text`). Compares every non-whitespace character, in order, across the spine,
    and checks the zip layout, XHTML well-formedness, the per-file size ceiling and that both TOCs resolve.
    """
    expected, removals = expected_book_text(normalized)
    try:
        archive = zipfile.ZipFile(io.BytesIO(epub))
    except zipfile.BadZipFile as exc:
        raise NovelError(f"Produced EPUB is not a valid zip: {exc}") from exc
    with archive:
        infos = archive.infolist()
        if (not infos or infos[0].filename != "mimetype" or infos[0].compress_type != zipfile.ZIP_STORED
                or archive.read("mimetype") != b"application/epub+zip"):
            raise NovelError("Produced EPUB does not start with a stored mimetype entry")
        try:
            package = ElementTree.fromstring(archive.read("OEBPS/content.opf"))
            manifest = {item.get("id"): item.get("href") for item in package.iterfind("o:manifest/o:item", OPF_NS)}
            spine = [manifest[ref.get("idref")] for ref in package.iterfind("o:spine/o:itemref", OPF_NS)]
            if package.findtext("o:metadata/{http://purl.org/dc/elements/1.1/}language", namespaces=OPF_NS) != LANGUAGE:
                raise NovelError("Produced EPUB lacks zh-CN language metadata")
            pieces: list[str] = []
            sizes: list[int] = []
            anchors: dict[str, set[str]] = {}
            for href in spine:
                data = archive.read(f"OEBPS/{href}")
                sizes.append(len(data))
                body = ElementTree.fromstring(data).find(f"{XHTML}body")
                if body is None:
                    raise NovelError(f"Spine item {href} has no body")
                ids = [node.get("id") for node in body.iter() if node.get("id") is not None]
                if len(ids) != len(set(ids)):
                    raise NovelError(f"Spine item {href} has duplicate fragment ids")
                anchors[href] = set(ids)
                pieces.append("".join("".join(body.itertext()).split()))
            nav = ElementTree.fromstring(archive.read("OEBPS/nav.xhtml"))
            ncx = ElementTree.fromstring(archive.read("OEBPS/toc.ncx"))
        except (ElementTree.ParseError, KeyError) as exc:
            raise NovelError(f"Produced EPUB is malformed: {exc}") from exc
    actual = "".join(pieces)
    if actual != expected:
        at = len(os.path.commonprefix([expected, actual]))
        raise NovelPreservationError(
            f"Text preservation check failed at non-whitespace character {at}: expected {expected[at:at + 12]!r}, "
            f"got {actual[at:at + 12]!r} (source {len(expected)} characters, EPUB {len(actual)})")
    if not sizes or max(sizes) > CEILING_FILE_BYTES:
        raise NovelError(f"Spine item of {max(sizes, default=0)} bytes exceeds the {CEILING_FILE_BYTES} byte ceiling")
    spine_set = set(spine)
    nav_targets = [a.get("href", "") for a in nav.iter(f"{XHTML}a")]
    ncx_targets = [c.get("src", "") for c in ncx.iter(f"{NCX}content")]
    if not nav_targets or nav_targets != ncx_targets:
        raise NovelError("Produced EPUB has inconsistent nav/NCX entries")
    for target in nav_targets:
        href, separator, anchor = target.partition("#")
        if href not in spine_set or (separator and anchor not in anchors[href]):
            raise NovelError("Produced EPUB has dangling nav/NCX entries")
    return {"files": len(sizes), "toc_entries": len(nav_targets), "max_file_bytes": max(sizes),
            "median_file_bytes": int(statistics.median(sizes)), **removals}


# ---------------------------------------------------------------------- conversion

@dataclass
class Converted:
    epub: bytes
    info: dict = field(default_factory=dict)


def convert_bytes(data: bytes, stem: str) -> Converted:
    """TXT bytes → verified EPUB bytes, or raise NovelError."""
    text, encoding, warnings = decode_text(data)
    normalized, dropped = normalize_text(text)
    paragraphs, headings, _ = read_structure(normalized)
    if not paragraphs:
        raise NovelError("The file contains no text")
    title, author = parse_filename(stem)
    if not author:
        for paragraph in paragraphs[:40]:
            match = _IN_FILE_AUTHOR.match(paragraph)
            if match:
                author = match.group(1).strip()
                break
    if dropped:
        warnings.append(f"Removed {dropped} control characters XML cannot represent")
    segments = segment(paragraphs, headings)
    plan = plan_pages(segments)
    if plan.paragraph_splits:
        warnings.append(f"{plan.paragraph_splits} paragraphs longer than {MAX_FILE_BYTES // 1000} KB were split at sentence ends")
    epub = build_novel_epub(title, author, plan, digest(data))
    stats = check_preservation(epub, normalized)
    info = {
        "title": title, "author": author, "encoding": encoding, "mode": plan.mode,
        "volumes": plan.volumes, "chapters": plan.chapters, "split_chapters": plan.split_segments,
        "front_matter": bool(segments and segments[0].level == 0 and plan.mode == "headings"),
        "removed_control_chars": dropped, "warnings": warnings, **stats,
    }
    return Converted(epub, info)


# ---------------------------------------------------------------------- tree + manifest

def _reject_nested_output(src: Path, out: Path) -> None:
    if out == src or src in out.parents:
        raise ReaderError(f"Output directory must be outside the source directory; refusing to write under {src}")


def discover(src: Path) -> list[Path]:
    found = []
    for directory, subdirectories, names in os.walk(src):
        subdirectories[:] = sorted(d for d in subdirectories if not d.startswith("."))
        found.extend(Path(directory) / n for n in sorted(names) if n.lower().endswith(".txt") and not n.startswith("."))
    return found


def load_manifest(out: Path) -> dict:
    path = out / MANIFEST_NAME
    if not path.exists():
        return {"version": MANIFEST_VERSION, "entries": {}}
    if path.is_symlink() or not path.is_file():
        raise ReaderError(f"Novel manifest is not a regular file: {path}")
    try:
        value = json.loads(bounded_read(path, MAX_MANIFEST_BYTES))
    except (OSError, ValueError, UnicodeError) as exc:
        raise ReaderError(f"Novel manifest is invalid: {path}: {exc}; rerun with --force to rebuild it") from None
    if not isinstance(value, dict) or value.get("version") != MANIFEST_VERSION or not isinstance(value.get("entries"), dict):
        raise ReaderError(f"Novel manifest has an unsupported schema: {path}; rerun with --force to rebuild it")
    return value


def save_manifest(out: Path, manifest: dict) -> None:
    payload = json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=1) + "\n"
    atomic_write(out / MANIFEST_NAME, payload.encode("utf-8"), overwrite=True)


def _convert_one(task: dict) -> dict:
    source = Path(task["path"])
    out_path = Path(task["out_path"])
    prior = task["prior"] or {}
    result = {"source": task["rel"], "output": task["out_rel"]}
    try:
        size = source.stat().st_size
        if size > MAX_SOURCE_BYTES:
            raise NovelError(f"Source is {size} bytes, over the {MAX_SOURCE_BYTES} byte limit")
        data = source.read_bytes()
        sha = digest(data)
        result.update(source_sha256=sha, source_bytes=len(data))
        prior_version = prior.get("converter_version")
        compatible = prior_version == CONVERTER_VERSION or (
            prior_version == UNGROUPED_VERSION and isinstance(prior.get("chapters"), int)
            and 0 <= prior["chapters"] <= GROUP_CHAPTER_THRESHOLD)
        if (not task["force"] and not task["stale"] and prior.get("status") == "ok" and prior.get("source_sha256") == sha
                and compatible and prior.get("output") == task["out_rel"]
                and out_path.is_file() and out_path.stat().st_size == prior.get("output_bytes")):
            return {**prior, **result, "status": "skipped"}
        book = convert_bytes(data, source.stem)
        atomic_write(out_path, book.epub, overwrite=True)
        result.update(book.info, status="converted", converter_version=CONVERTER_VERSION, text_clean=TEXT_CLEAN_REVISION,
                      output_sha256=digest(book.epub), output_bytes=len(book.epub))
    except ReaderError as exc:
        result.update(status="failed", error=str(exc))
    except OSError as exc:
        result.update(status="failed", error=f"I/O error: {exc}")
    except Exception as exc:  # a converter bug must fail that one book loudly, not the batch
        result.update(status="failed", error=f"internal error: {type(exc).__name__}: {exc}")
    return result


def _manifest_entry(record: dict) -> dict:
    keep = ("status", "output", "source_sha256", "source_bytes", "converter_version", "text_clean", "output_sha256", "output_bytes",
            "title", "author", "encoding", "mode", "volumes", "chapters", "split_chapters", "front_matter", "files",
            "toc_entries", "max_file_bytes", "median_file_bytes", "removed_control_chars", "removed_decoration_chars",
            "decoration_lines_dropped", "headings_cleaned", "duplicate_headings_dropped", "duplicate_heading_chars_removed",
            "warnings", "error")
    return {key: record[key] for key in keep if key in record}


def xhtml_sizes(out: Path, manifest: dict) -> list[int]:
    """Uncompressed size of every spine XHTML across converted books (zip directories only)."""
    sizes = []
    for entry in manifest["entries"].values():
        path = out / entry.get("output", "")
        if entry.get("status") != "ok" or not path.is_file():
            continue
        with zipfile.ZipFile(path) as archive:
            sizes.extend(i.file_size for i in archive.infolist() if re.fullmatch(r"OEBPS/s\d{5}\.xhtml", i.filename))
    return sizes


def default_jobs() -> int:
    return max(1, min(6, (os.cpu_count() or 2) - 1))


def convert_tree(src: Path, out: Path, *, force: bool = False, jobs: int = 1,
                 rebuild_stale: bool = False,
                 progress: Callable[[int, int, dict], None] | None = None) -> dict:
    """Convert every .txt below `src` into a mirrored .epub below `out`. `src` is only ever read.

    ``rebuild_stale`` reconverts entries whose recorded ``text_clean`` revision is missing or
    older than ``TEXT_CLEAN_REVISION`` even when their source is unchanged; ``force`` rebuilds
    everything instead.
    """
    src = Path(src).expanduser().resolve()
    out = Path(out).expanduser().resolve()
    if not src.is_dir():
        raise ReaderError(f"Source directory not found: {src}")
    _reject_nested_output(src, out)
    out.mkdir(parents=True, exist_ok=True)
    try:
        manifest = load_manifest(out)
    except ReaderError:
        if not force:
            raise
        manifest = {"version": MANIFEST_VERSION, "entries": {}}
    entries: dict = manifest["entries"]
    files = discover(src)
    tasks: list[dict] = []
    collisions: list[dict] = []
    claimed: dict[str, str] = {}
    for path in files:
        rel = path.relative_to(src).as_posix()
        out_rel = (path.parent.relative_to(src) / output_name(path.stem)).as_posix()
        if out_rel in claimed:
            collisions.append({"source": rel, "output": out_rel, "status": "failed",
                               "error": f"Output name collides with the conversion of {claimed[out_rel]}"})
            continue
        claimed[out_rel] = rel
        prior = entries.get(rel)
        tasks.append({"path": str(path), "rel": rel, "out_rel": out_rel, "out_path": str(out / out_rel),
                      "prior": prior, "force": force,
                      "stale": bool(rebuild_stale and (prior or {}).get("text_clean") != TEXT_CLEAN_REVISION)})
    total = len(tasks) + len(collisions)
    done = 0
    since_save = 0
    records: list[dict] = []

    def finish(record: dict) -> None:
        nonlocal done, since_save
        done += 1
        records.append(record)
        entry = _manifest_entry(record)
        if record["status"] in ("converted", "skipped"):
            entry["status"] = "ok"
        entries[record["source"]] = entry
        since_save += 1
        if since_save >= 25:
            save_manifest(out, manifest)
            since_save = 0
        if progress:
            progress(done, total, record)

    for record in collisions:
        finish(record)
    if jobs > 1 and len(tasks) > 1:
        with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as pool:
            for future in concurrent.futures.as_completed([pool.submit(_convert_one, t) for t in tasks]):
                finish(future.result())
    else:
        for task in tasks:
            finish(_convert_one(task))
    present = {p.relative_to(src).as_posix() for p in files}
    orphaned = sorted(rel for rel in entries if rel not in present)
    save_manifest(out, manifest)
    records.sort(key=lambda r: r["source"])
    sizes = xhtml_sizes(out, manifest)
    ok = [r for r in records if r["status"] in ("converted", "skipped")]
    summary = {
        "source": str(src), "output": str(out), "total": len(records),
        "converted": sum(r["status"] == "converted" for r in records),
        "skipped": sum(r["status"] == "skipped" for r in records),
        "failed": sum(r["status"] == "failed" for r in records),
        "orphaned": orphaned,
        "headings_mode": sum(r.get("mode") == "headings" for r in ok),
        "size_mode": sum(r.get("mode") == "size" for r in ok),
        "books_with_size_split_chapters": sum(bool(r.get("split_chapters")) for r in ok),
        "removed_decoration_chars": sum(r.get("removed_decoration_chars", 0) for r in ok),
        "decoration_lines_dropped": sum(r.get("decoration_lines_dropped", 0) for r in ok),
        "headings_cleaned": sum(r.get("headings_cleaned", 0) for r in ok),
        "duplicate_headings_dropped": sum(r.get("duplicate_headings_dropped", 0) for r in ok),
        "duplicate_heading_chars_removed": sum(r.get("duplicate_heading_chars_removed", 0) for r in ok),
        "xhtml_files": len(sizes),
        "xhtml_median_bytes": int(statistics.median(sizes)) if sizes else 0,
        "xhtml_max_bytes": max(sizes, default=0),
        "output_bytes": sum(r.get("output_bytes", 0) for r in ok),
    }
    report = {"summary": summary, "books": records}
    atomic_write(out / REPORT_NAME, (json.dumps(report, ensure_ascii=False, indent=1) + "\n").encode("utf-8"), overwrite=True)
    return report


def deliverable_outputs(out: Path) -> tuple[list[dict], list[str]]:
    """Manifest-tracked EPUBs whose bytes still match their recorded SHA-256, plus problems for the rest."""
    out = Path(out).expanduser().resolve()
    if not (out / MANIFEST_NAME).is_file():
        raise ReaderError(f"No novel manifest in {out}; run `reader-send novels convert` first")
    manifest = load_manifest(out)
    items: list[dict] = []
    problems: list[str] = []
    for entry in manifest["entries"].values():
        if entry.get("status") != "ok":
            continue
        path = out / entry["output"]
        if path.is_symlink() or not path.is_file():
            problems.append(f"{entry['output']}: converted EPUB is missing")
            continue
        data = path.read_bytes()
        if digest(data) != entry["output_sha256"]:
            problems.append(f"{entry['output']}: EPUB no longer matches its recorded SHA-256; reconvert it")
            continue
        items.append({"rel": entry["output"], "path": path, "sha256": entry["output_sha256"], "size": len(data)})
    items.sort(key=lambda item: item["rel"])
    return items, sorted(problems)
