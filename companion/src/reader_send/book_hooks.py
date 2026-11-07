"""Front-card "sharpest passages" trailer for real EPUB books.

Long books open on a cover and a table of contents, so the strongest argument stays buried five
chapters in.  This module scans the *body* XHTML of an ordinary EPUB, scores self-contained passages
for claim density, contrast and aphoristic punch, then inserts one generated front card at the head of
the spine.  Every passage on the card links straight to its paragraph in the source chapter.

The rewrite is intentionally conservative: original ZIP entries keep their order, compression and
timestamps; only the package document, the selected chapter files and the new card entry change; the
first in-place run leaves a backup beside the book, and later runs replace the card in place instead
of stacking duplicates.
"""
from __future__ import annotations

import hashlib
import html
import os
import posixpath
import re
import tempfile
import zipfile
from dataclasses import dataclass, field
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote
from xml.etree import ElementTree

from .common import ReaderError, digest

CARD_ITEM_ID = "reader-hooks-sharpest"
CARD_FILENAME = "reader-hooks-sharpest.xhtml"
CARD_TITLE = "Sharpest Passages in This Book"
CARD_MARKER = "reader-hooks:sharpest:v1"
ANCHOR_PREFIX = "rw-hook-"
BACKUP_SUFFIX = ".pre-hooks.epub"

DEFAULT_COUNT = 5
DEFAULT_MINIMUM = 4
MAX_BOOK_BYTES = 512 * 1024 * 1024
MAX_ENTRY_BYTES = 32 * 1024 * 1024

MIN_BLOCK_TEXT = 60
MAX_BLOCK_TEXT = 1200
MIN_PASSAGE = 70
MAX_PASSAGE = 420
MAX_WINDOW = 380
MIN_SCORE = 2.0

BLOCK_TAGS = ("p",)
FALLBACK_BLOCK_TAGS = ("div", "blockquote", "li")

FRONT_HINTS = (
    "cover", "titlepage", "title-page", "title_page", "halftitle", "half-title", "copyright",
    "dedication", "epigraph", "frontmatter", "front-matter", "imprint", "colophon", "toc",
    "contents", "nav", "praise", "alsoby", "also-by", "alsoread", "advert", "promo",
)
BACK_HINTS = (
    "index", "endnotes", "footnotes", "notes", "bibliography", "glossary", "appendix",
    "acknowledg", "abouttheauthor", "about-the-author", "backmatter", "back-matter", "credits",
)
MATTER_HINTS = FRONT_HINTS + BACK_HINTS
SKIP_TAGS = {"head", "script", "style", "title"}
HEADING_TAGS = {"h1", "h2", "h3", "h4"}

CLAIM_MARKERS = (
    "must", "cannot", "can not", "never", "always", "every", "nobody", "no one", "nothing",
    "none", "only", "in truth", "the fact that", "means that", "proves", "therefore", "thus",
    "inevitable", "impossible", "necessary", "precisely", "by definition", "the whole point",
    "is not", "are not", "was not", "there is no", "nothing but",
)
CONTRAST_MARKERS = (
    "but", "yet", "however", "nevertheless", "nonetheless", "rather", "instead", "whereas",
    "on the contrary", "and yet", "even so", "in reality", "the truth is", "what is worse",
    "although", "though", "not merely", "quite the contrary",
)
NARRATIVE_MARKERS = (
    "had been", "was founded", "were taught", "was born", "he was", "she was", "they were",
    "in the year", "at the age of", "in 1", "in 2", "memorandum", "letter", "wrote to",
)
HIGH_VOLTAGE = (
    "truth", "power", "nature", "man", "men", "soul", "god", "freedom", "virtue", "reason",
    "justice", "love", "death", "fear", "war", "peace", "history", "mind", "evil", "good",
    "authority", "liberty", "desire", "will", "faith", "error", "illusion", "necessity",
)
PUNCH_MARKERS = (
    "no ", "never", "nothing", "every", "all ", "only", "must", "cannot", "is not", "are not",
    "will not", "does not", "which is why", "that is why", "the reason",
)
BORING_MARKERS = (
    "chapter", "figure", "table", "page ", "ibid", "op. cit", "cf.", "isbn", "copyright",
    "press", "translat", "edition", "volume ", "see ", "http", "www.", "e-mail",
)
ABBREVIATIONS = {"mr", "mrs", "ms", "dr", "st", "cf", "e.g", "i.e", "vol", "ch", "p", "pp",
                 "no", "nos", "fig", "ca", "c", "vs", "jr", "sr", "prof", "rev"}

_SENTENCE_END = re.compile(r"(?<=[.!?])(?=[\"'”’)\]]*[\s\u00a0]+[\"“'‘(\[]?[A-Z0-9])")
_WS = re.compile(r"[\s\u00a0]+")
_TOKEN = re.compile(r"[^\W_]+", re.UNICODE)
_ATTR = re.compile(r"""([\w:.-]+)\s*=\s*(?:"([^"]*)"|'([^']*)')""")
_ITEM = re.compile(r"<(?:\w+:)?item\b([^>]*?)/?>", re.IGNORECASE)
_ITEMREF = re.compile(r"<(?:\w+:)?itemref\b([^>]*?)/?>", re.IGNORECASE)
_SPINE_OPEN = re.compile(r"<(?:\w+:)?spine\b[^>]*>", re.IGNORECASE)
_MANIFEST_CLOSE = re.compile(r"</(?:\w+:)?manifest\s*>", re.IGNORECASE)


@dataclass(frozen=True)
class Score:
    """Explainable passage score; tests assert on the individual signals."""

    total: float
    claims: int
    contrasts: int
    punch: int
    penalties: tuple[str, ...] = ()

    @property
    def reasons(self) -> tuple[str, ...]:
        return (f"claims={self.claims}", f"contrasts={self.contrasts}",
                f"punch={self.punch}", *self.penalties)


@dataclass(frozen=True)
class Block:
    """One candidate paragraph in one XHTML document."""

    index: int
    tag: str
    start: int
    text: str
    existing_id: str | None
    line_breaks: int


@dataclass(frozen=True)
class Document:
    href: str
    path: str                  # ZIP entry name
    title: str
    text: str
    blocks: tuple[Block, ...]
    position: int
    eligible: bool
    reason: str = ""


@dataclass
class Passage:
    """A selected passage and the anchor that makes it tappable."""

    text: str
    score: Score
    doc_path: str
    doc_href: str
    chapter: str
    block_index: int
    start: int
    anchor: str = ""

    def as_dict(self) -> dict:
        return {"text": self.text, "chapter": self.chapter, "href": self.doc_href,
                "anchor": self.anchor, "score": self.score.total,
                "signals": list(self.score.reasons)}


@dataclass
class HookResult:
    book: Path
    output: Path
    backup: Path | None
    passages: list[Passage]
    changed: bool
    card: str
    documents: int
    sha256: str
    skipped: list[str] = field(default_factory=list)

    def as_dict(self) -> dict:
        return {"book": str(self.book), "output": str(self.output),
                "backup": str(self.backup) if self.backup else None,
                "card": self.card, "changed": self.changed,
                "documents": self.documents, "sha256": self.sha256,
                "passages": [passage.as_dict() for passage in self.passages],
                "skipped": list(self.skipped)}


# --------------------------------------------------------------------------------------
# Text scoring
# --------------------------------------------------------------------------------------
def sentences(text: str) -> list[str]:
    """Split prose into sentences without a heavyweight NLP dependency."""
    text = _WS.sub(" ", text).strip()
    if not text:
        return []
    merged: list[str] = []
    for part in _SENTENCE_END.split(text):
        part = part.strip()
        if not part:
            continue
        if merged:
            previous = merged[-1]
            tail = previous.rsplit(" ", 1)[-1].rstrip(".").lower()
            if tail in ABBREVIATIONS or previous.endswith(("Mr.", "Mrs.", "Dr.", "St.")):
                merged[-1] = previous + " " + part
                continue
        merged.append(part)
    return merged


def words(text: str) -> list[str]:
    return [match.group(0).lower() for match in _TOKEN.finditer(text)]


def _has(lower: str, marker: str) -> bool:
    if marker.endswith(" "):
        return marker in lower
    return re.search(rf"(?<![^\W_]){re.escape(marker)}(?![^\W_])", lower) is not None


def score_text(text: str) -> Score:
    """Score one self-contained passage. Higher is more provocative and quotable."""
    clean = _WS.sub(" ", text).strip()
    lower = clean.lower()
    length = len(clean)
    penalties: list[str] = []
    total = 0.0

    if 80 <= length <= 320:
        total += 2.4
    elif 66 <= length <= MAX_PASSAGE:
        total += 1.2
    elif length < 60:
        total -= 3.0
        penalties.append("too-short")
    else:
        total -= 1.4
        penalties.append("overlong")

    claims = sum(1 for marker in CLAIM_MARKERS if _has(lower, marker))
    contrasts = sum(1 for marker in CONTRAST_MARKERS if _has(lower, marker))
    punch = sum(1 for marker in PUNCH_MARKERS if _has(lower, marker))
    total += min(claims, 5) * 0.85
    total += min(contrasts, 3) * 0.9
    total += min(punch, 4) * 0.55
    if re.search(r"(?<![\w])not\b[^.]{0,80}\bbut\b", lower):
        total += 1.1
        contrasts += 1
    if re.search(r"\b(is|are|was|were|becomes?)\b[^.]{0,70}\b(not|never|nothing|impossible)\b", lower):
        total += 0.5
    high_voltage = sum(1 for marker in HIGH_VOLTAGE if _has(lower, marker))
    total += min(high_voltage, 4) * 0.3
    narrative = sum(1 for marker in NARRATIVE_MARKERS if marker in lower)
    if narrative:
        total -= min(narrative, 3) * 1.1
        penalties.append("narrative")
    if re.search(r"\bnot only\b", lower):
        total -= 0.9
        penalties.append("enumerative")
    if re.match(r"[a-z]", clean):
        total -= 2.0
        penalties.append("mid-sentence")

    digits = sum(char.isdigit() for char in clean)
    if length and digits / length > 0.04:
        total -= 1.8
        penalties.append("digits")
    if re.search(r"\b(isbn|copyright|all rights reserved|printed in|translated by)\b", lower):
        total -= 3.0
        penalties.append("imprint")
    boring = sum(1 for marker in BORING_MARKERS if marker in lower)
    if boring:
        total -= min(boring, 3) * 1.0
        penalties.append("apparatus")
    if "http" in lower or "www." in lower:
        total -= 4.0
        penalties.append("url")
    if clean.endswith("?"):
        total -= 1.2
        penalties.append("question")
    elif "?" in clean:
        total -= 0.4
    if len(re.findall(r"\b[A-Z]{3,}\b", clean)) > 2:
        total -= 1.2
        penalties.append("caps")
    if clean.count("(") >= 3:
        total -= 0.9
        penalties.append("parenthetical")
    if clean.count("“") + clean.count('"') >= 4:
        total -= 0.7
        penalties.append("dialogue")
    if not re.search(r"[.!?][\"'”’)\]]?$", clean):
        total -= 1.0
        penalties.append("unterminated")

    return Score(round(total, 3), claims, contrasts, punch, tuple(penalties))


# --------------------------------------------------------------------------------------
# XHTML scanning
# --------------------------------------------------------------------------------------
class _Scanner(HTMLParser):
    """Collect text blocks plus the byte offsets needed for surgical anchor insertion."""

    def __init__(self, raw: str, block_tags: tuple[str, ...]):
        super().__init__(convert_charrefs=True)
        self.raw = raw
        self.block_tags = frozenset(block_tags)
        self.line_offsets = [0]
        for match in re.finditer("\n", raw):
            self.line_offsets.append(match.end())
        self.blocks: list[Block] = []
        self.headings: list[tuple[str, str]] = []
        self.title = ""
        self.body_text: list[str] = []
        self._in_body = False
        self._skip = 0
        self._capture: dict | None = None
        self._sup: list[str] | None = None
        self._heading: tuple[str, list[str]] | None = None

    def _offset(self) -> int:
        line, column = self.getpos()
        if 0 < line <= len(self.line_offsets):
            return self.line_offsets[line - 1] + column
        return len(self.raw)

    def _append(self, text: str) -> None:
        if not text:
            return
        if self._sup is not None:
            self._sup.append(text)
            return
        targets = []
        if self._capture is not None:
            targets.append(self._capture["parts"])
        if self._heading is not None:
            targets.append(self._heading[1])
        if self._in_body and not self._skip:
            targets.append(self.body_text)
        for parts in targets:
            # Inline markup (``<span>``) splits words with no whitespace in the source.
            if parts and parts[-1] and parts[-1][-1].isalnum() and text[0].isalnum():
                parts.append(" ")
            parts.append(text)

    def handle_starttag(self, tag, attrs):
        tag = tag.lower()
        if tag in SKIP_TAGS:
            self._skip += 1
            return
        if tag == "sup":
            self._sup = []
            return
        if tag == "body":
            self._in_body = True
        if tag in HEADING_TAGS:
            self._heading = (tag, [])
        if tag in self.block_tags:
            if self._capture is not None:
                self._finish_block()
            attributes = {key.lower(): (value or "") for key, value in attrs}
            start = self._offset()
            if start < len(self.raw) and self.raw[start] == "<":
                self._capture = {"tag": tag, "start": start, "parts": [],
                                 "id": attributes.get("id") or attributes.get("xml:id") or None,
                                 "breaks": 0}

    def handle_startendtag(self, tag, attrs):
        tag = tag.lower()
        if tag == "br":
            if self._capture is not None:
                self._capture["parts"].append("\n")
                self._capture["breaks"] += 1
            return
        self.handle_starttag(tag, attrs)

    def handle_endtag(self, tag):
        tag = tag.lower()
        if tag in SKIP_TAGS:
            self._skip = max(0, self._skip - 1)
            return
        if tag == "sup":
            text = "".join(self._sup or []).strip()
            self._sup = None
            if text and not re.fullmatch(r"[\dIVXLCivxlc]{1,5}", text):
                self._append(text)
            return
        if tag in HEADING_TAGS and self._heading is not None:
            text = _WS.sub(" ", "".join(self._heading[1])).strip()
            if text:
                self.headings.append((tag, text))
            self._heading = None
        if tag in self.block_tags and self._capture is not None and self._capture["tag"] == tag:
            self._finish_block()

    def handle_data(self, data):
        self._append(data)

    def _finish_block(self):
        capture, self._capture = self._capture, None
        text = _WS.sub(" ", "".join(capture["parts"])).strip()
        if len(text) < MIN_BLOCK_TEXT:
            return
        self.blocks.append(Block(len(self.blocks), capture["tag"], capture["start"], text,
                                 capture["id"], capture["breaks"]))

    @property
    def full_text(self) -> str:
        return _WS.sub(" ", "".join(self.body_text)).strip()


def _decode(raw: bytes, name: str) -> str:
    for encoding in ("utf-8-sig", "utf-8"):
        try:
            return raw.decode(encoding)
        except UnicodeDecodeError:
            continue
    declared = re.search(rb"encoding\s*=\s*[\"']([\w.-]+)[\"']", raw[:400])
    if declared:
        try:
            return raw.decode(declared.group(1).decode("ascii"))
        except (LookupError, UnicodeDecodeError):
            pass
    try:
        return raw.decode("cp1252")
    except UnicodeDecodeError as exc:
        raise ReaderError(f"Chapter {name} is not valid UTF-8 or Windows-1252 text") from exc


# --------------------------------------------------------------------------------------
# Package parsing
# --------------------------------------------------------------------------------------
def _read_package(archive: zipfile.ZipFile) -> tuple[str, str]:
    try:
        container = archive.read("META-INF/container.xml")
    except KeyError as exc:
        raise ReaderError("EPUB is missing META-INF/container.xml") from exc
    try:
        root = ElementTree.fromstring(container)
    except ElementTree.ParseError as exc:
        raise ReaderError(f"EPUB container.xml is not valid XML: {exc}") from exc
    rootfile = next((node for node in root.iter() if node.tag.rsplit("}", 1)[-1] == "rootfile"), None)
    opf = unquote((rootfile.get("full-path") if rootfile is not None else "") or "").lstrip("/")
    if not opf or opf not in archive.namelist() or ".." in Path(opf).parts:
        raise ReaderError("EPUB container.xml does not point at a readable package document")
    return opf, _decode(archive.read(opf), opf)


def _attributes(fragment: str) -> dict[str, str]:
    return {key.lower(): (double if double is not None else single or "")
            for key, double, single in _ATTR.findall(fragment)}


def _opf_items(opf_text: str) -> dict[str, dict]:
    items: dict[str, dict] = {}
    for match in _ITEM.finditer(opf_text):
        attributes = _attributes(match.group(1))
        if attributes.get("id"):
            items[attributes["id"]] = attributes
    return items


def _spine(opf_text: str) -> list[dict[str, str]]:
    return [attributes for attributes in
            (_attributes(match.group(1)) for match in _ITEMREF.finditer(opf_text))
            if attributes.get("idref")]


def _zip_path(opf: str, href: str) -> str:
    return posixpath.normpath(posixpath.join(posixpath.dirname(opf), unquote(href)))


def _hint_tokens(path: str) -> list[str]:
    return [token for token in re.split(r"[^a-z0-9]+", posixpath.basename(path).lower()) if token]


def _classify(path: str, text: str, position: int, spine_size: int,
              headings: list[str]) -> tuple[bool, str]:
    """Return (eligible, reason). Front/back matter, apparatus and stub pages are excluded."""
    tokens = _hint_tokens(path)
    if "split" not in tokens:      # calibre ``index_split_000.html`` is body text, not an index
        hint = "-".join(tokens)
        if any(item in hint for item in MATTER_HINTS):
            return False, "front-or-back-matter"
    heading = " ".join(headings).lower()
    chapter_like = bool(re.search(r"(chapter|part|book|lecture|essay|section|act|letter)",
                                  heading))
    if re.search(r"\b(index|contents|copyright|dedication|colophon|bibliography|glossary|"
                 r"appendix|appendices|translations?|sources|acknowledg\w*|foreword|preface)\b", heading):
        return False, "apparatus-heading"
    if len(text) < 400:
        return False, "too-short"
    if (spine_size >= 6 and position < max(1, int(spine_size * 0.06))
            and len(text) < 1200 and not chapter_like):
        return False, "front-matter"
    opening = text[:600].lower()
    if len(text) < 6000 and re.search(r"\b(index|bibliography|endnotes|glossary)\b", opening) \
            and "index of" not in opening:
        return False, "apparatus-text"
    return True, ""


def _document_title(headings: list[tuple[str, str]], markup: str, fallback: str) -> str:
    for tag, heading in headings:
        if tag in ("h1", "h2", "h3") and not re.fullmatch(
                r"(chapter|part|book)?\s*[ivxlcdm\d]+", heading.strip(), re.IGNORECASE):
            return heading
    if headings:
        return headings[0][1]
    match = re.search(r"<title[^>]*>(.*?)</title>", markup, re.IGNORECASE | re.DOTALL)
    if match:
        title = _WS.sub(" ", re.sub(r"<[^>]+>", " ", match.group(1))).strip()
        if title and not re.fullmatch(r"[\w-]{1,12}", title):
            return html.unescape(title)
    # A reader-visible label is better than ``index_split_004``; the caller inherits the
    # previous chapter's title for continuation files.
    return "" if re.search(r"(split|part)[-_]?\d", fallback, re.IGNORECASE) else fallback


def read_book(path: Path) -> tuple[zipfile.ZipFile, str, str, list[Document]]:
    """Parse a book into ordered spine documents. The caller owns the archive handle."""
    try:
        archive = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError) as exc:
        raise ReaderError(f"Not a readable EPUB: {path}: {exc}") from exc
    try:
        opf, opf_text = _read_package(archive)
        items = _opf_items(opf_text)
        spine = _spine(opf_text)
        if not spine:
            raise ReaderError("EPUB package document has no spine")
        # A previously injected front card is output, not source: keep it out of the reading order
        # so that re-running sees exactly the documents the first run saw.
        spine = [ref for ref in spine
                 if posixpath.basename(_zip_path(opf, items.get(ref.get("idref", ""), {})
                                                 .get("href", ""))) != CARD_FILENAME]
        entries = set(archive.namelist())
        documents: list[Document] = []
        previous_title = ""
        chapter_number = 0
        for position, ref in enumerate(spine):
            item = items.get(ref.get("idref", ""))
            if not item or not item.get("href"):
                continue
            media = (item.get("media-type") or "application/xhtml+xml").lower()
            if media not in ("application/xhtml+xml", "text/html", "application/x-dtbook+xml"):
                continue
            zip_name = _zip_path(opf, item["href"])
            if zip_name not in entries or archive.getinfo(zip_name).file_size > MAX_ENTRY_BYTES:
                continue
            markup = _decode(archive.read(zip_name), zip_name)
            blocks = _scan_blocks(markup, BLOCK_TAGS)
            if not blocks:
                for fallback in FALLBACK_BLOCK_TAGS:
                    candidate = _scan_blocks(markup, (fallback,))
                    if len(candidate) > len(blocks):
                        blocks = candidate
            scanner = _Scanner(markup, BLOCK_TAGS)
            scanner.feed(markup)
            body = scanner.full_text
            title = _document_title(scanner.headings, markup,
                                    posixpath.splitext(posixpath.basename(zip_name))[0])
            if not title:
                split_like = re.search(r"(split|part)[-_]?\d", posixpath.basename(zip_name),
                                       re.IGNORECASE)
                if split_like and previous_title and not previous_title.startswith("Chapter "):
                    title = previous_title
                else:
                    chapter_number += 1
                    title = f"Chapter {chapter_number}"
            previous_title = title
            eligible, reason = _classify(zip_name, body, position, len(spine),
                                         [heading for _, heading in scanner.headings])
            documents.append(Document(item["href"], zip_name, title, body, tuple(blocks),
                                      position, eligible, reason))
        return archive, opf, opf_text, documents
    except Exception:
        archive.close()
        raise


def _scan_blocks(markup: str, tags: tuple[str, ...]) -> list[Block]:
    scanner = _Scanner(markup, tags)
    scanner.feed(markup)
    return scanner.blocks


# --------------------------------------------------------------------------------------
# Selection
# --------------------------------------------------------------------------------------
def _clean_passage(text: str) -> str:
    """Drop orphan closing punctuation left behind when a window starts mid-quotation."""
    text = _WS.sub(" ", text).strip()
    while text and text[0] in "\"'”’)]}»":
        text = text[1:].lstrip()
    while text and text[-1] in "\"'“‘([{«":
        text = text[:-1].rstrip()
    return text


def _window_candidates(block: Block) -> list[tuple[str, float]]:
    """Candidate passages from a block: the whole paragraph or a strong 1–3 sentence window."""
    text = _clean_passage(block.text)
    if len(text) <= MAX_PASSAGE:
        return [(text, 0.0)] if len(text) >= MIN_PASSAGE else []
    parts = [_clean_passage(part) for part in sentences(text)]
    parts = [part for part in parts if part]
    candidates: list[tuple[str, float]] = []
    for start in range(len(parts)):
        for size in (1, 2, 3):
            end = start + size
            if end > len(parts):
                break
            window = _clean_passage(" ".join(parts[start:end]))
            if len(window) > MAX_WINDOW:
                break
            if len(window) < MIN_PASSAGE:
                continue
            broken = 0.0 if re.search(r"[.!?][\"'”’)\]]?$", window) else 0.4
            candidates.append((window, broken))
    return candidates


def _select(documents: list[Document], count: int, minimum: int) -> list[Passage]:
    eligible = [document for document in documents if document.eligible and document.blocks]
    scored: list[tuple[float, Passage]] = []
    for document in eligible:
        for block in document.blocks:
            for text, broken in _window_candidates(block):
                score = score_text(text)
                total = score.total - broken
                if total < MIN_SCORE:
                    continue
                scored.append((total, Passage(text, score, document.path, document.href,
                                              document.title, block.index, block.start)))
    scored.sort(key=lambda item: (-item[0], item[1].doc_path, item[1].start))

    per_doc = 2 if len(eligible) > 1 else count
    chosen: list[Passage] = []
    used_docs: dict[str, int] = {}
    used_blocks: set[tuple[str, int]] = set()
    seen: set[str] = set()
    for _, passage in scored:
        if len(chosen) >= count:
            break
        key = digest(_WS.sub(" ", passage.text.lower()))[:32]
        block_key = (passage.doc_path, passage.block_index)
        if key in seen or block_key in used_blocks or used_docs.get(passage.doc_path, 0) >= per_doc:
            continue
        chosen.append(passage)
        used_blocks.add(block_key)
        used_docs[passage.doc_path] = used_docs.get(passage.doc_path, 0) + 1
        seen.add(key)

    if len(chosen) < minimum:
        raise ReaderError(f"Only {len(chosen)} self-contained passage(s) scored high enough; "
                          f"lower --minimum or inspect the book")
    if len(used_docs) < min(3, len(eligible)):
        raise ReaderError(f"Selected passages cover only {len(used_docs)} chapter file(s); "
                          "refusing a one-chapter trailer")
    chosen.sort(key=lambda passage: (-passage.score.total, passage.doc_path, passage.start))
    counter = 0
    for passage in chosen:
        document = next(doc for doc in documents if doc.path == passage.doc_path)
        existing = document.blocks[passage.block_index].existing_id
        if existing:
            passage.anchor = existing
        else:
            counter += 1
            passage.anchor = f"{ANCHOR_PREFIX}{counter}"
    return chosen


def select_passages(path: Path, *, count: int = DEFAULT_COUNT,
                    minimum: int = DEFAULT_MINIMUM) -> list[Passage]:
    """Choose 4–5 self-contained passages spread across body chapters (read-only)."""
    _check_count(count, minimum)
    archive, _, _, documents = read_book(Path(path).expanduser())
    archive.close()
    return _select(documents, count, minimum)


def _check_count(count: int, minimum: int) -> None:
    if type(count) is not int or not 1 <= count <= 12:
        raise ReaderError("--count must be an integer from 1 to 12")
    if type(minimum) is not int or not 0 <= minimum <= count:
        raise ReaderError("--minimum must be an integer from 0 to --count")


# --------------------------------------------------------------------------------------
# Rewrite
# --------------------------------------------------------------------------------------
def _insert_anchor(markup: str, start: int, anchor: str) -> str:
    """Add an id to the selected paragraph's start tag, leaving everything else untouched."""
    if start >= len(markup) or markup[start] != "<":
        raise ReaderError("Selected paragraph offset does not point at a start tag")
    tag_end = _tag_end_at(markup, start)
    if tag_end < 0:
        raise ReaderError("Malformed HTML start tag in the selected chapter")
    head = markup[start:tag_end]
    if re.search(r"""\bid\s*=""", head):
        return markup
    closing = "/" if head.endswith("/") else ""
    body = head[:-1] if closing else head
    return f'{markup[:start]}{body} id="{html.escape(anchor, quote=True)}"{closing}>{markup[tag_end + 1:]}'


def _tag_end_at(raw: str, start: int) -> int:
    quote = ""
    index = start
    while index < len(raw):
        char = raw[index]
        if quote:
            if char == quote:
                quote = ""
        elif char in "\"'":
            quote = char
        elif char == ">":
            return index
        index += 1
    return -1


def _card_xhtml(passages: list[Passage], card_path: str) -> str:
    items = []
    for passage in passages:
        target = posixpath.relpath(passage.doc_path, posixpath.dirname(card_path) or ".")
        href = f"{target}#{passage.anchor}"
        items.append(
            f'<li><a href="{html.escape(href, quote=True)}">{html.escape(passage.text)}</a>'
            f'<p class="where">{html.escape(passage.chapter)}</p></li>')
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en"><head><title>{html.escape(CARD_TITLE)}</title>
<meta name="generator" content="{CARD_MARKER}" />
<style type="text/css">body{{line-height:1.4;margin:1em}}h1{{font-size:1.3em}}.intro{{font-style:italic}}
ol{{padding-left:1.2em}}li{{margin:0 0 1.1em 0}}a{{text-decoration:none}}
.where{{margin:.2em 0 0 .1em;font-size:.75em;font-style:italic}}</style></head>
<body><!-- {CARD_MARKER} -->
<h1>{html.escape(CARD_TITLE)}</h1>
<p class="intro">The lines worth arguing with. Tap any passage to open the chapter it came from.</p>
<ol>
{chr(10).join(items)}
</ol>
</body></html>
'''


def _strip_card(opf_text: str) -> str:
    opf_text = re.sub(rf"<!--\s*{re.escape(CARD_MARKER)}\s*-->\s*", "", opf_text)
    opf_text = re.sub(rf"""<(?:\w+:)?item\b[^>]*\bid\s*=\s*["']{re.escape(CARD_ITEM_ID)}["'][^>]*?/?>""",
                      "", opf_text)
    opf_text = re.sub(rf"""<(?:\w+:)?itemref\b[^>]*\bidref\s*=\s*["']{re.escape(CARD_ITEM_ID)}["'][^>]*?/?>""",
                      "", opf_text)
    return opf_text


def _rewrite_opf(opf_text: str, card_href: str) -> str:
    """Insert the card manifest item and put it first in the spine; idempotent by construction."""
    opf_text = _strip_card(opf_text)
    item = (f'<!-- {CARD_MARKER} --><item id="{CARD_ITEM_ID}" '
            f'href="{html.escape(card_href, quote=True)}" media-type="application/xhtml+xml"/>')
    close = _MANIFEST_CLOSE.search(opf_text)
    if not close:
        raise ReaderError("EPUB package document has no manifest element")
    opf_text = opf_text[:close.start()] + item + opf_text[close.start():]
    spine = _SPINE_OPEN.search(opf_text)
    if not spine:
        raise ReaderError("EPUB package document has no spine element")
    reference = f'<itemref idref="{CARD_ITEM_ID}"/>'
    return opf_text[:spine.end()] + reference + opf_text[spine.end():]


def _write_epub(source: Path, destination: Path, replacements: dict[str, bytes],
                additions: dict[str, bytes], insert_after: str | None) -> None:
    """Copy every original entry in order, swapping only the named ones."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".reader-hooks-", dir=destination.parent)
    os.close(fd)
    try:
        with zipfile.ZipFile(source) as original, zipfile.ZipFile(temporary, "w") as target:
            names = original.namelist()
            if not names or names[0] != "mimetype":
                raise ReaderError("EPUB archive does not start with a mimetype entry")
            written: set[str] = set()

            def add(name: str, data: bytes, original_info: zipfile.ZipInfo | None) -> None:
                info = zipfile.ZipInfo(name, date_time=original_info.date_time if original_info
                                       else (1980, 1, 1, 0, 0, 0))
                if name == "mimetype":
                    info.compress_type = zipfile.ZIP_STORED
                elif original_info is not None and original_info.compress_type in (
                        zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                    info.compress_type = original_info.compress_type
                else:
                    info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (original_info.external_attr if original_info else 0) or (0o600 << 16)
                if original_info is not None:
                    info.create_system = original_info.create_system
                target.writestr(info, data)
                written.add(name)

            for name in names:
                info = original.getinfo(name)
                add(name, replacements.get(name, original.read(name)), info)
                if insert_after and name == insert_after:
                    for extra, payload in additions.items():
                        if extra not in written and extra not in names:
                            add(extra, payload, None)
            for extra, payload in additions.items():
                if extra not in written:
                    add(extra, payload, None)
        with open(temporary, "rb+") as stream:
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def _copy_file(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".reader-hooks-", dir=destination.parent)
    try:
        with os.fdopen(fd, "wb") as output, source.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def apply_hooks(path: Path, output: Path | None = None, *, count: int = DEFAULT_COUNT,
                minimum: int = DEFAULT_MINIMUM, backup: bool = True) -> HookResult:
    """Insert (or refresh) the sharpest-passages front card in one EPUB."""
    _check_count(count, minimum)
    source = Path(path).expanduser()
    if source.is_symlink():
        raise ReaderError("EPUB input must not be a symlink")
    source = source.resolve()
    if not source.is_file():
        raise ReaderError(f"EPUB not found: {source}")
    if source.stat().st_size > MAX_BOOK_BYTES:
        raise ReaderError(f"EPUB exceeds the {MAX_BOOK_BYTES // (1024 * 1024)} MiB safety limit")
    destination = Path(output).expanduser().resolve() if output else source
    if destination.is_symlink():
        raise ReaderError("EPUB output must not be a symlink")
    if destination.suffix.lower() != ".epub":
        raise ReaderError("Output must keep the .epub extension")

    archive, opf, opf_text, documents = read_book(source)
    try:
        passages = _select(documents, count, minimum)

        card_path = posixpath.join(posixpath.dirname(opf), CARD_FILENAME)
        card_href = posixpath.relpath(card_path, posixpath.dirname(opf) or ".")
        replacements: dict[str, bytes] = {}
        by_doc: dict[str, list[Passage]] = {}
        for passage in passages:
            by_doc.setdefault(passage.doc_path, []).append(passage)
        for doc_path, doc_passages in by_doc.items():
            markup = _decode(archive.read(doc_path), doc_path)
            for passage in sorted(doc_passages, key=lambda item: item.start, reverse=True):
                markup = _insert_anchor(markup, passage.start, passage.anchor)
            replacements[doc_path] = markup.encode("utf-8")
        rewritten_opf = _rewrite_opf(opf_text, card_href)
        if rewritten_opf != opf_text:
            replacements[opf] = rewritten_opf.encode("utf-8")
        card = _card_xhtml(passages, card_path).encode("utf-8")
        existing = set(archive.namelist())
        if card_path in existing:
            replacements[card_path] = card
            additions: dict[str, bytes] = {}
        else:
            additions = {card_path: card}

        changed = any(archive.read(name) != data for name, data in replacements.items())
        skipped = [f"{doc.path}: {doc.reason}" for doc in documents if not doc.eligible]
        backup_path = None
        if changed:
            if backup:
                candidate = source.with_name(source.stem + BACKUP_SUFFIX)
                if not candidate.exists():
                    _copy_file(source, candidate)
                backup_path = candidate
            _write_epub(source, destination, replacements, additions, insert_after=opf)
        elif destination != source:
            _copy_file(source, destination)
        elif backup:
            candidate = source.with_name(source.stem + BACKUP_SUFFIX)
            backup_path = candidate if candidate.exists() else None
        result_sha = hashlib.sha256(destination.read_bytes()).hexdigest()
    finally:
        archive.close()
    return HookResult(source, destination, backup_path, passages, changed, card_path,
                      sum(1 for doc in documents if doc.eligible), result_sha, skipped)
