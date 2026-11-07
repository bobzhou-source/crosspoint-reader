"""Cross-Fire marginalia: pair reader annotations with replies from Robert's Substack archive.

When a highlight, question or "review later" mark comes back from the card, this module finds the
sharpest passages in the local Substack corpus that agree with, extend or flatly contradict it, and
builds one small EPUB card ("Cross-Fire: Marginalia & Replies") for ``/books/Inbox`` so the argument
continues on the reader.

Retrieval stays lightweight.  The archive is chunked into paragraph-sized passages and kept in a
persistent SQLite FTS5 index (built once, refreshed only for changed files); queries use FTS5's BM25
ranking, then a token-overlap pass and a debate-marker boost pick the excerpt.  Nothing here needs
torch, embeddings or a server, and a small pure-Python BM25 index is available when SQLite lacks
FTS5.
"""
from __future__ import annotations

import ast
import hashlib
import html
import json
import math
import os
import re
import sqlite3
import tempfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

from .book_hooks import score_text
from .common import ReaderError, atomic_write, bounded_read, digest
from .epub import CONTAINER_XML, pack_epub

DEFAULT_CORPUS = Path.home() / "substack-corpus"
DEFAULT_NEIGHBORS = Path.home() / "substack-neighbors" / "posts"
DEFAULT_DB = Path(os.environ.get("READER_SEND_CROSSFIRE_DB")
                  or Path.home() / ".cache" / "reader-send" / "crossfire.sqlite3")
CARD_TITLE = "Cross-Fire: Marginalia & Replies"
CARD_PREFIX = "crossfire"
CARD_LIMIT_ANNOTATIONS = 12
MAX_ANNOTATION_CHARS = 4000
MAX_POST_BYTES = 4 * 1024 * 1024
MAX_JSONL_BYTES = 512 * 1024 * 1024
MIN_CHUNK = 140
MAX_CHUNK = 900
MAX_EXCERPT = 420
DEFAULT_CANDIDATES = 400
DEFAULT_REPLIES = 3
MIN_CHUNK_HITS = 2
BM25_K1 = 1.2
BM25_B = 0.75

STOPWORDS = frozenset("""
a about above after again against all am an and any are aren't as at be because been before being
below between both but by can cannot could couldn't did didn't do does doesn't doing don't down
during each few for from further had hadn't has hasn't have haven't having he he'd he'll he's her
here here's hers herself him himself his how how's i i'd i'll i'm i've if in into is isn't it it's
its itself let's me more most mustn't my myself no nor not of off on once only or other ought our
ours ourselves out over own same shan't she she'd she'll she's should shouldn't so some such than
that that's the their theirs them themselves then there there's these they they'd they'll they're
they've this those through to too under until up very was wasn't we we'd we'll we're we've were
weren't what what's when when's where where's which while who who's whom why why's with won't
would wouldn't you you'd you'll you're you've your yours yourself yourselves also just like one
two would may might much many even still get got make made say says said thing things way ways
really quite rather perhaps though although however
""".split())

DEBATE_MARKERS = (
    "wrong", "false", "myth", "nonsense", "absurd", "actually", "in fact", "on the contrary",
    "but ", "however", "yet ", "no one", "everyone", "nobody", "the problem", "i think",
    "i suspect", "the truth", "lie", "dishonest", "contradiction", "cannot", "never", "must",
    "should", "insist", "object", "disagree", "refute", "mistake", "error", "delusion",
    "propaganda",
)

_MD_NAME = re.compile(r"^(?P<date>\d{8})_(?P<time>\d{6})_(?P<slug>.+)\.md$")
_TOKEN = re.compile(r"[^\W_]+", re.UNICODE)
_SENTENCE_END = re.compile(r"(?<=[.!?])(?=[\"'”’)\]]*[\s\u00a0]+[\"“'‘(\[]?[A-Z0-9])")
_WS = re.compile(r"[\s\u00a0]+")
_MD_IMAGE = re.compile(r"!\[[^\]]*\]\([^)]*\)")
_MD_LINK = re.compile(r"\[([^\]]+)\]\([^)]*\)")
_HTML_TAG = re.compile(r"<[^>]+>")
ELLIPSIS = re.compile(r"(?:…|\.\.\.)")
BOILERPLATE = (
    "is a reader-supported publication",
    "to receive new posts and support my work",
    "this post is for paid subscribers",
    "share this post",
    "leave a comment",
    "subscribe now",
)
KIND_NAMES = ("quote", "question", "review_later")


# --------------------------------------------------------------------------------------
# Records
# --------------------------------------------------------------------------------------
@dataclass(frozen=True)
class Annotation:
    book: str
    selection: str
    note: str
    kind: str
    created: str
    key: str

    @property
    def query(self) -> str:
        return f"{self.selection} {self.note}".strip()

    def as_dict(self) -> dict:
        return {"book": self.book, "kind": self.kind, "created": self.created,
                "selection": self.selection, "note": self.note}


@dataclass(frozen=True)
class Post:
    author: str
    title: str
    date: str
    url: str
    kind: str
    text: str

    @property
    def key(self) -> str:
        return f"{self.kind}:{self.author}:{self.title}"


@dataclass(frozen=True)
class Chunk:
    post: Post
    text: str


@dataclass(frozen=True)
class Reply:
    score: float
    overlap: int
    excerpt: str
    chunk: Chunk

    def as_dict(self) -> dict:
        return {"author": self.chunk.post.author, "title": self.chunk.post.title,
                "date": self.chunk.post.date, "url": self.chunk.post.url,
                "kind": self.chunk.post.kind, "excerpt": self.excerpt,
                "score": round(self.score, 4), "overlap": self.overlap}


# --------------------------------------------------------------------------------------
# Tokens and text
# --------------------------------------------------------------------------------------
def content_tokens(text: str) -> list[str]:
    tokens = []
    for match in _TOKEN.finditer(text):
        token = match.group(0).lower()
        if len(token) < 3 or token in STOPWORDS or token.isdigit():
            continue
        if len(token) > 5 and token.endswith("'s"):
            token = token[:-2]
        elif len(token) > 4 and token.endswith("ies"):
            token = token[:-3] + "y"
        elif len(token) > 4 and token.endswith("s") and not token.endswith("ss"):
            token = token[:-1]
        tokens.append(token)
    return tokens


def sentences(text: str) -> list[str]:
    text = _WS.sub(" ", text).strip()
    return [part.strip() for part in _SENTENCE_END.split(text) if part.strip()]


def _overlap(text: str, terms: set[str]) -> int:
    return sum(1 for token in content_tokens(text) if token in terms)


# --------------------------------------------------------------------------------------
# Loading the local archive
# --------------------------------------------------------------------------------------
def _md_body(markup: str) -> tuple[str, str, str]:
    """Return (title, url, body) from one archived Markdown post."""
    title = ""
    url = ""
    kept: list[str] = []
    for line in markup.splitlines():
        stripped = line.strip()
        if not title and stripped.startswith("# "):
            title = stripped[2:].strip()
            continue
        if stripped.startswith("original content:"):
            url = stripped.split(":", 1)[1].strip()
            continue
        lowered = stripped.lower()
        if any(marker in lowered for marker in BOILERPLATE):
            continue
        if not stripped or stripped.startswith("[!["):
            kept.append("")
            continue
        text = _MD_IMAGE.sub("", stripped)
        text = _MD_LINK.sub(r"\1", text)
        text = _HTML_TAG.sub(" ", text)
        text = text.replace("**", "").replace("__", "").strip()
        kept.append(text)
    return title, url, "\n".join(kept)


def parse_markdown(path: Path, publication: str) -> Post | None:
    match = _MD_NAME.match(path.name)
    if not match:
        return None
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    if len(raw) > MAX_POST_BYTES:
        return None
    title, url, body = _md_body(raw)
    if not body.strip():
        return None
    date = datetime.strptime(match.group("date"), "%Y%m%d").date().isoformat()
    if not title:
        title = match.group("slug").replace("-", " ").strip().title()
    return Post(publication, title, date, url, "substack", body)


def _author(value, fallback: str) -> str:
    if isinstance(value, list):
        return str(value[0]) if value else fallback
    if isinstance(value, str) and value.strip():
        text = value.strip()
        if text.startswith("["):
            try:
                parsed = ast.literal_eval(text)
            except (ValueError, SyntaxError):
                parsed = None
            if isinstance(parsed, (list, tuple)) and parsed:
                return str(parsed[0])
        return text
    return fallback


def parse_jsonl_post(record: dict, fallback: str) -> Post | None:
    text = record.get("text") or record.get("body") or ""
    if not isinstance(text, str) or len(text.strip()) < MIN_CHUNK:
        return None
    title = record.get("title") or record.get("slug") or ""
    if not isinstance(title, str):
        return None
    date = record.get("post_date") or record.get("date") or ""
    if isinstance(date, str) and len(date) >= 10:
        date = date[:10]
    author = _author(record.get("authors") or record.get("author"),
                     record.get("host") or fallback)
    url = record.get("url") if isinstance(record.get("url"), str) else ""
    return Post(author, title.strip() or "Untitled", date, url, "substack", text)


def source_files(corpus: Path | None, neighbors: Path | None,
                 authors: set[str] | None = None):
    """Yield (file key, kind, path, publication) for every archived source file."""
    if corpus is not None and Path(corpus).is_dir():
        for path in sorted(Path(corpus).rglob("*.md")):
            relative = path.relative_to(corpus)
            if any(part.startswith("_") for part in relative.parts[:-1]):
                continue
            publication = relative.parts[0] if len(relative.parts) > 1 else ""
            if authors and publication not in authors:
                continue
            yield str(path), "md", path, publication
    if neighbors is not None and Path(neighbors).is_dir():
        for path in sorted(Path(neighbors).glob("*.jsonl")):
            if authors and path.stem not in authors:
                continue
            yield str(path), "jsonl", path, path.stem


def file_posts(kind: str, path: Path, publication: str) -> list[Post]:
    if kind == "md":
        post = parse_markdown(path, publication)
        return [post] if post is not None else []
    posts: list[Post] = []
    total = 0
    try:
        with path.open(encoding="utf-8", errors="replace") as stream:
            for line in stream:
                total += len(line)
                if total > MAX_JSONL_BYTES:
                    break
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                try:
                    record = json.loads(line)
                except ValueError:
                    continue
                if not isinstance(record, dict):
                    continue
                post = parse_jsonl_post(record, publication)
                if post is not None:
                    posts.append(post)
    except OSError:
        return []
    return posts


def iter_posts(corpus: Path | None = DEFAULT_CORPUS, neighbors: Path | None = DEFAULT_NEIGHBORS,
               authors: set[str] | None = None):
    for _, kind, path, publication in source_files(corpus, neighbors, authors):
        yield from file_posts(kind, path, publication)


def _fragments(text: str) -> list[str]:
    """Split on ellipses: the archive stores some long posts truncated mid-sentence."""
    pieces = []
    for fragment in ELLIPSIS.split(text):
        fragment = _WS.sub(" ", fragment).strip()
        if len(fragment) >= 60:
            pieces.append(fragment)
    return pieces


def chunk_post(post: Post) -> list[Chunk]:
    chunks: list[Chunk] = []
    for fragment in _fragments(post.text):
        for paragraph in re.split(r"\n\s*\n", fragment):
            paragraph = _WS.sub(" ", paragraph).strip()
            if len(paragraph) < MIN_CHUNK:
                continue
            if len(paragraph) <= MAX_CHUNK:
                chunks.append(Chunk(post, paragraph))
                continue
            current = ""
            for sentence in sentences(paragraph):
                if len(current) + len(sentence) + 1 <= MAX_CHUNK:
                    current = f"{current} {sentence}".strip()
                    continue
                if len(current) >= MIN_CHUNK:
                    chunks.append(Chunk(post, current))
                current = sentence
            if len(current) >= MIN_CHUNK:
                chunks.append(Chunk(post, current))
    return chunks


# --------------------------------------------------------------------------------------
# Persistent FTS5 index
# --------------------------------------------------------------------------------------
SCHEMA = """
CREATE TABLE IF NOT EXISTS files (path TEXT PRIMARY KEY, signature TEXT NOT NULL);
CREATE VIRTUAL TABLE IF NOT EXISTS chunks USING fts5(
    text, author UNINDEXED, title UNINDEXED, date UNINDEXED, url UNINDEXED,
    kind UNINDEXED, file UNINDEXED
);
"""


class CorpusStore:
    """SQLite FTS5 index over paragraph-sized chunks of the local archive.

    The first refresh indexes the archive (~15–30 s for ~38k posts); later runs only touch files
    whose size or mtime changed, so a query costs milliseconds.
    """

    def __init__(self, path: Path | str = DEFAULT_DB):
        self.path = Path(path)
        if str(self.path) != ":memory:":
            self.path.parent.mkdir(parents=True, exist_ok=True)
            if self.path.is_symlink():
                raise ReaderError(f"Cross-Fire index path must not be a symlink: {self.path}")
        try:
            self.db = sqlite3.connect(str(self.path))
            self.db.executescript(SCHEMA)
        except sqlite3.OperationalError as exc:  # pragma: no cover - depends on build flags
            raise ReaderError(f"SQLite FTS5 is unavailable: {exc}") from exc

    def close(self) -> None:
        self.db.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.close()

    def _signature(self, path: Path) -> str:
        info = path.stat()
        return f"{info.st_mtime_ns}:{info.st_size}"

    def refresh(self, corpus: Path | None = DEFAULT_CORPUS,
                neighbors: Path | None = DEFAULT_NEIGHBORS,
                authors: set[str] | None = None) -> dict:
        known = dict(self.db.execute("SELECT path, signature FROM files"))
        seen: set[str] = set()
        indexed = removed = changed = 0
        self.db.execute("PRAGMA synchronous=OFF")
        try:
            for key, kind, path, publication in source_files(corpus, neighbors, authors):
                seen.add(key)
                try:
                    signature = self._signature(path)
                except OSError:
                    continue
                if known.get(key) == signature:
                    continue
                changed += 1
                self.db.execute("DELETE FROM chunks WHERE file = ?", (key,))
                rows = []
                for post in file_posts(kind, path, publication):
                    indexed += 1
                    for chunk in chunk_post(post):
                        rows.append((chunk.text, post.author, post.title, post.date, post.url,
                                     post.kind, key))
                if rows:
                    self.db.executemany(
                        "INSERT INTO chunks (text, author, title, date, url, kind, file) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?)", rows)
                self.db.execute("INSERT OR REPLACE INTO files (path, signature) VALUES (?, ?)",
                                (key, signature))
            for key in set(known) - seen:
                self.db.execute("DELETE FROM chunks WHERE file = ?", (key,))
                self.db.execute("DELETE FROM files WHERE path = ?", (key,))
                removed += 1
        finally:
            self.db.commit()
            self.db.execute("PRAGMA synchronous=NORMAL")
        return {"changed": changed, "removed": removed, "posts": indexed}

    def count(self) -> int:
        return self.db.execute("SELECT count(*) FROM chunks").fetchone()[0]

    def search(self, terms: set[str], limit: int = DEFAULT_CANDIDATES) -> list[tuple[float, Chunk]]:
        """BM25-ranked chunks; FTS5 returns a lower-is-better negative score."""
        terms = {term for term in terms if term and re.fullmatch(r"[\w']+", term)}
        if not terms:
            return []
        query = " OR ".join(f'"{term}"' for term in sorted(terms))
        rows = self.db.execute(
            "SELECT text, author, title, date, url, kind, bm25(chunks) AS rank FROM chunks "
            "WHERE chunks MATCH ? ORDER BY rank LIMIT ?", (query, limit)).fetchall()
        results = []
        for text, author, title, date, url, kind, rank in rows:
            results.append((-float(rank), Chunk(Post(author, title, date, url, kind, text), text)))
        return results


# --------------------------------------------------------------------------------------
# In-memory BM25 (fallback and small fixtures)
# --------------------------------------------------------------------------------------
class BM25Index:
    """Pure-Python BM25 with a token-overlap tiebreak."""

    def __init__(self, chunks: list[Chunk], k1: float = BM25_K1, b: float = BM25_B):
        self.chunks = chunks
        self.k1 = k1
        self.b = b
        self._counts: list[dict[str, int]] = []
        self._lengths: list[int] = []
        self._postings: dict[str, dict[int, int]] = {}
        for position, chunk in enumerate(chunks):
            tokens = content_tokens(chunk.text)
            counts: dict[str, int] = {}
            for token in tokens:
                counts[token] = counts.get(token, 0) + 1
            self._counts.append(counts)
            self._lengths.append(len(tokens))
            for token, count in counts.items():
                self._postings.setdefault(token, {})[position] = count
        self._average = (sum(self._lengths) / len(self._lengths)) if self._lengths else 0.0

    def __len__(self) -> int:
        return len(self.chunks)

    def scores(self, terms: set[str]) -> dict[int, tuple[float, int]]:
        total = len(self.chunks)
        results: dict[int, tuple[float, int]] = {}
        for term in terms:
            posting = self._postings.get(term)
            if not posting:
                continue
            document_frequency = len(posting)
            idf = math.log(1 + (total - document_frequency + 0.5) / (document_frequency + 0.5))
            for position, frequency in posting.items():
                length = self._lengths[position]
                denominator = frequency + self.k1 * (
                    1 - self.b + self.b * (length / self._average if self._average else 0))
                if denominator <= 0:
                    continue
                score, overlap = results.get(position, (0.0, 0))
                results[position] = (score + idf * frequency * (self.k1 + 1) / denominator,
                                     overlap + 1)
        return results

    def search(self, terms: set[str], limit: int = 20) -> list[tuple[float, int, Chunk]]:
        ranked = []
        for position, (score, overlap) in self.scores(terms).items():
            if overlap == 0:
                continue
            chunk = self.chunks[position]
            boost = sum(1 for marker in DEBATE_MARKERS if marker in chunk.text.lower())
            ranked.append((score + min(boost, 4) * 0.12, overlap, chunk))
        ranked.sort(key=lambda item: (-item[0], -item[1], item[2].post.key, item[2].text[:40]))
        return ranked[:limit]


def prefilter(terms: set[str], *, corpus: Path | None = DEFAULT_CORPUS,
              neighbors: Path | None = DEFAULT_NEIGHBORS, limit: int = DEFAULT_CANDIDATES,
              authors: set[str] | None = None) -> list[Post]:
    """One streaming pass: keep the posts with the strongest token overlap for the query."""
    terms = {term for term in terms if term}
    if not terms:
        return []
    kept: list[tuple[float, int, str, Post]] = []
    for post in iter_posts(corpus, neighbors, authors):
        tokens = content_tokens(post.text)
        hits = sum(1 for token in tokens if token in terms)
        if hits < MIN_CHUNK_HITS:
            continue
        score = hits / math.sqrt(max(len(tokens), 1))
        record = (score, hits, f"{post.kind}:{post.author}:{post.title}", post)
        if len(kept) < limit:
            kept.append(record)
            if len(kept) == limit:
                kept.sort(key=lambda item: (item[0], item[1], item[2]))
        elif record[:3] > kept[0][:3]:
            kept[0] = record
            kept.sort(key=lambda item: (item[0], item[1], item[2]))
    return [item[3] for item in sorted(kept, key=lambda item: (-item[0], -item[1], item[2]))]


def index_from_candidates(candidates: list[Post], annotations: list[Annotation]) -> BM25Index:
    terms: set[str] = set()
    for annotation in annotations:
        terms.update(content_tokens(annotation.query))
    chunks: list[Chunk] = []
    for post in candidates:
        for chunk in chunk_post(post):
            if _overlap(chunk.text, terms) >= MIN_CHUNK_HITS:
                chunks.append(chunk)
    return BM25Index(chunks)


# --------------------------------------------------------------------------------------
# Excerpts and matching
# --------------------------------------------------------------------------------------
def best_excerpt(text: str, terms: set[str], limit: int = MAX_EXCERPT) -> str:
    parts = [part for part in sentences(text) if part]
    best = ""
    best_score = -1e9
    for start in range(len(parts)):
        for size in (1, 2, 3):
            end = start + size
            if end > len(parts):
                break
            window = " ".join(parts[start:end]).strip()
            if len(window) > limit:
                break
            if len(window) < 70:
                continue
            hits = _overlap(window, terms)
            if hits == 0:
                continue
            score = hits * 1.4 + score_text(window).total
            if score > best_score:
                best, best_score = window, score
    return best or text[:limit].strip()


def select_replies(annotation: Annotation, candidates: list[tuple[float, Chunk]],
                   *, replies: int = DEFAULT_REPLIES) -> list[Reply]:
    terms = set(content_tokens(annotation.query))
    ranked = []
    for score, chunk in candidates:
        overlap = _overlap(chunk.text, terms)
        if overlap < MIN_CHUNK_HITS:
            continue
        boost = sum(1 for marker in DEBATE_MARKERS if marker in chunk.text.lower())
        ranked.append((score + min(boost, 4) * 0.12, overlap, chunk))
    ranked.sort(key=lambda item: (-item[0], -item[1], item[2].post.key, item[2].text[:40]))

    chosen: list[Reply] = []
    deferred: list[Reply] = []
    seen_sources: set[str] = set()
    seen_excerpts: set[str] = set()
    for score, overlap, chunk in ranked:
        excerpt = best_excerpt(chunk.text, terms)
        if not excerpt:
            continue
        fingerprint = digest(excerpt.lower())[:16]
        if fingerprint in seen_excerpts or chunk.post.key in seen_sources:
            continue
        reply = Reply(round(score, 6), overlap, excerpt, chunk)
        if chunk.post.author in {item.chunk.post.author for item in chosen}:
            deferred.append(reply)
            continue
        chosen.append(reply)
        seen_sources.add(chunk.post.key)
        seen_excerpts.add(fingerprint)
        if len(chosen) >= replies:
            break
    for reply in deferred:
        if len(chosen) >= replies:
            break
        fingerprint = digest(reply.excerpt.lower())[:16]
        if fingerprint in seen_excerpts or reply.chunk.post.key in seen_sources:
            continue
        chosen.append(reply)
        seen_sources.add(reply.chunk.post.key)
        seen_excerpts.add(fingerprint)
    return chosen


def match_annotation(annotation: Annotation, index: BM25Index, *,
                     replies: int = DEFAULT_REPLIES) -> list[Reply]:
    terms = set(content_tokens(annotation.query))
    candidates = [(score, chunk) for score, _, chunk in index.search(terms, limit=replies * 30)]
    return select_replies(annotation, candidates, replies=replies)


# --------------------------------------------------------------------------------------
# Annotations
# --------------------------------------------------------------------------------------
def _iso(milliseconds) -> str:
    if isinstance(milliseconds, (int, float)) and milliseconds > 0:
        try:
            return datetime.fromtimestamp(milliseconds / 1000, tz=timezone.utc).date().isoformat()
        except (OverflowError, OSError, ValueError):
            return ""
    return ""


def _kind(value) -> str:
    if type(value) is int:
        return KIND_NAMES[value] if 0 <= value <= 2 else "quote"
    if isinstance(value, str):
        name = value.removeprefix("AnnotationKind::").strip().lower().replace(" ", "_")
        if name in KIND_NAMES:
            return name
        if name.replace("_", "") == "reviewlater":
            return "review_later"
    return "quote"


def normalize_annotation(value: dict) -> Annotation | None:
    """Accept native card records and mapped/cloud annotation records alike."""
    if not isinstance(value, dict):
        return None
    native = "sourcePath" in value
    book = value.get("sourcePath") or value.get("book_path") or value.get("book") or ""
    selection = value.get("excerpt") if native else value.get("selection")
    if selection is None:
        selection = value.get("text") or value.get("highlight") or ""
    note = value.get("note") or ""
    if not isinstance(selection, str):
        selection = str(selection)
    if not isinstance(note, str):
        note = str(note)
    selection = _WS.sub(" ", selection).strip()[:MAX_ANNOTATION_CHARS]
    note = _WS.sub(" ", note).strip()[:MAX_ANNOTATION_CHARS]
    if not selection and not note:
        return None
    if native:
        created = value.get("createdAt")
        created = _iso(created * 1000 if isinstance(created, (int, float)) else None)
    else:
        created = _iso(value.get("created_at"))
    annotation = Annotation(str(book or "Untitled"), selection, note, _kind(value.get("kind")),
                            created, key="")
    return Annotation(annotation.book, annotation.selection, annotation.note, annotation.kind,
                      annotation.created,
                      digest(f"{annotation.book}\n{annotation.selection}\n{annotation.note}")[:16])


def normalize_annotations(values) -> list[Annotation]:
    if isinstance(values, dict):
        values = values.get("annotations") if isinstance(values.get("annotations"), list) else [values]
    if not isinstance(values, list):
        raise ReaderError("Annotations must be a JSON list or an object with an annotations list")
    result: list[Annotation] = []
    seen: set[str] = set()
    for value in values:
        if isinstance(value, Annotation):
            annotation = value
        else:
            annotation = normalize_annotation(value)
        if annotation is None or annotation.key in seen:
            continue
        seen.add(annotation.key)
        result.append(annotation)
    result.sort(key=lambda item: (item.created or "0000", item.key), reverse=True)
    return result


def load_annotation_file(path: Path) -> list[Annotation]:
    path = Path(path).expanduser()
    if path.is_dir():
        values = []
        for pattern in ("*.json", "*.ndjson", "*.jsonl"):
            for file in sorted(path.rglob(pattern)):
                if file.name.startswith("._"):
                    continue
                loaded = _load_json(file)
                if isinstance(loaded, dict):
                    values.append(loaded)
                elif isinstance(loaded, list):
                    values.extend(loaded)
        return normalize_annotations(values)
    return normalize_annotations(_load_json(path))


def _load_json(path: Path):
    raw = bounded_read(path, 64 * 1024 * 1024)
    try:
        return json.loads(raw)
    except ValueError:
        values = []
        for line in raw.splitlines():
            line = line.strip()
            if line:
                try:
                    values.append(json.loads(line))
                except ValueError:
                    continue
        return values


# --------------------------------------------------------------------------------------
# Card
# --------------------------------------------------------------------------------------
def _escape(value: str) -> str:
    return html.escape(value, quote=True)


def _section(annotation: Annotation, replies: list[Reply]) -> str:
    pieces = [
        '<section class="annotation">',
        f'<h2>{_escape(annotation.book)}</h2>',
        f'<p class="kind">{_escape(annotation.kind.replace("_", " "))}'
        + (f' · {_escape(annotation.created)}' if annotation.created else "") + "</p>",
    ]
    if annotation.selection:
        pieces.append(f'<blockquote class="mine">{_escape(annotation.selection)}</blockquote>')
    if annotation.note:
        pieces.append(f'<p class="note"><strong>Your note:</strong> {_escape(annotation.note)}</p>')
    pieces.append("<h3>Cross-fire</h3>")
    if not replies:
        pieces.append('<p class="empty">No matching Substack passage yet.</p>')
    for reply in replies:
        post = reply.chunk.post
        meta = " · ".join(part for part in (post.author, post.date) if part)
        pieces.extend([
            '<article class="reply">',
            f'<h4>{_escape(post.title)}</h4>',
            f'<p class="meta">{_escape(meta)}</p>',
            f'<blockquote>{_escape(reply.excerpt)}</blockquote>',
        ])
        if post.url:
            pieces.append(f'<p class="source">{_escape(post.url)}</p>')
        pieces.append("</article>")
    pieces.append("</section>")
    return "\n".join(pieces)


def build_card(sections: list[tuple[Annotation, list[Reply]]], *, title: str = CARD_TITLE,
               identifier: str | None = None) -> bytes:
    body = "\n".join(_section(annotation, replies) for annotation, replies in sections)
    xhtml = f'''<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en"><head><title>{_escape(title)}</title>
<meta name="generator" content="crossfire:v1" />
<link rel="stylesheet" type="text/css" href="style.css" /></head><body>
<h1>{_escape(title)}</h1>
<p class="intro">Your marginalia, answered by the writers you already read. Excerpts come from the
local Substack archive; titles, authors and dates are the originals.</p>
{body}
</body></html>'''
    uid = "urn:sha256:" + digest(identifier or body)
    opf = f'''<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="crossfire-id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="crossfire-id">{uid}</dc:identifier>
<dc:title>{_escape(title)}</dc:title><dc:language>en</dc:language>
<dc:creator>reader-send crossfire</dc:creator></metadata>
<manifest><item id="card" href="card.xhtml" media-type="application/xhtml+xml"/>
<item id="style" href="style.css" media-type="text/css"/>
<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest>
<spine toc="ncx"><itemref idref="card"/></spine></package>'''
    ncx = f'''<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head>
<meta name="dtb:uid" content="{uid}"/><meta name="dtb:depth" content="1"/>
<meta name="dtb:totalPageCount" content="0"/><meta name="dtb:maxPageNumber" content="0"/></head>
<docTitle><text>{_escape(title)}</text></docTitle><navMap><navPoint id="card" playOrder="1">
<navLabel><text>{_escape(title)}</text></navLabel><content src="card.xhtml"/></navPoint></navMap></ncx>'''
    style = ("body { line-height: 1.4; margin: 1em; } h1 { font-size: 1.4em; } "
             "section { margin-bottom: 1.6em; } blockquote { margin: .5em 0 .5em .6em; "
             "padding-left: .6em; border-left: 2px solid #888; font-style: italic; } "
             ".mine { font-style: normal; font-weight: bold; } "
             ".meta, .kind, .source { font-size: .8em; color: #444; } "
             ".empty { font-style: italic; }")
    return pack_epub({
        "mimetype": "application/epub+zip",
        "META-INF/container.xml": CONTAINER_XML,
        "OEBPS/content.opf": opf,
        "OEBPS/toc.ncx": ncx,
        "OEBPS/card.xhtml": xhtml,
        "OEBPS/style.css": style,
    })


def card_filename(data: bytes) -> str:
    return f"{CARD_PREFIX}-{hashlib.sha256(data).hexdigest()[:12]}.epub"


def build_sections(annotations: list[Annotation], *, corpus: Path | None = DEFAULT_CORPUS,
                   neighbors: Path | None = DEFAULT_NEIGHBORS, replies: int = DEFAULT_REPLIES,
                   store: CorpusStore | None = None, limit: int = DEFAULT_CANDIDATES,
                   authors: set[str] | None = None) -> list[tuple[Annotation, list[Reply]]]:
    annotations = annotations[:CARD_LIMIT_ANNOTATIONS]
    terms: set[str] = set()
    for annotation in annotations:
        terms.update(content_tokens(annotation.query))
    if store is not None:
        store.refresh(corpus, neighbors, authors)
        candidates = store.search(terms, limit=limit)
        return [(annotation, select_replies(annotation, candidates, replies=replies))
                for annotation in annotations]
    posts = prefilter(terms, corpus=corpus, neighbors=neighbors, limit=limit, authors=authors)
    index = index_from_candidates(posts, annotations)
    return [(annotation, match_annotation(annotation, index, replies=replies))
            for annotation in annotations]


def write_card(inbox: Path | None, annotations, *, corpus: Path | None = DEFAULT_CORPUS,
               neighbors: Path | None = DEFAULT_NEIGHBORS, replies: int = DEFAULT_REPLIES,
               db: Path | str | None = DEFAULT_DB, limit: int = DEFAULT_CANDIDATES,
               authors: set[str] | None = None, dry_run: bool = False,
               out: Path | None = None) -> dict:
    """Build the Cross-Fire card for one or more imported annotations."""
    if type(replies) is not int or not 1 <= replies <= 6:
        raise ReaderError("--replies must be an integer from 1 to 6")
    if type(limit) is not int or not 1 <= limit <= 5000:
        raise ReaderError("--limit must be an integer from 1 to 5000")
    records = normalize_annotations(annotations)
    if not records:
        return {"status": "empty", "reason": "no annotations with a passage or note", "written": False}
    store = None
    refresh = {}
    try:
        if db is not None:
            store = CorpusStore(db)
    except (ReaderError, sqlite3.Error, OSError):
        store = None
    try:
        sections = build_sections(records, corpus=corpus, neighbors=neighbors, replies=replies,
                                  store=store, limit=limit, authors=authors)
    finally:
        if store is not None:
            store.close()
    data = build_card(sections)
    name = card_filename(data)
    result = {
        "status": "ok",
        "filename": name,
        "written": False,
        "index": "fts5" if store is not None else "bm25",
        "annotations": len(sections),
        "matches": sum(len(section_replies) for _, section_replies in sections),
        "sections": [{"annotation": annotation.as_dict(),
                      "replies": [reply.as_dict() for reply in section_replies]}
                     for annotation, section_replies in sections],
    }
    if dry_run:
        return result
    destination = Path(out).expanduser() if out else Path(inbox).expanduser() / name
    if destination.is_symlink():
        raise ReaderError("Cross-Fire card path must not be a symlink")
    if destination.exists() and destination.is_file() and destination.read_bytes() == data:
        result["path"] = str(destination)
        return result
    destination.parent.mkdir(parents=True, exist_ok=True)
    atomic_write(destination, data, overwrite=True)
    result["path"] = str(destination)
    result["written"] = True
    return result


# --------------------------------------------------------------------------------------
# CLI glue
# --------------------------------------------------------------------------------------
def load_from_card(volume_root: Path) -> list[Annotation]:
    store = Path(volume_root) / ".crosspoint" / "annotations"
    if not store.is_dir():
        return []
    values = []
    for file in sorted(store.rglob("*.json")):
        if file.name.startswith("._"):
            continue
        try:
            loaded = _load_json(file)
        except (OSError, ReaderError):
            continue
        if isinstance(loaded, dict):
            values.append(loaded)
        elif isinstance(loaded, list):
            values.extend(loaded)
    return normalize_annotations(values)


def run(args) -> dict:
    annotations_path = getattr(args, "annotations", None)
    inbox = getattr(args, "inbox", None)
    out = getattr(args, "out", None)
    if annotations_path:
        annotations = load_annotation_file(Path(annotations_path))
    else:
        from . import card as card_module

        config_path = getattr(args, "config", None) or card_module.default_config_path()
        config = card_module.load_config(config_path)
        volume = card_module.resolve_card(config["volume_uuid"])
        annotations = load_from_card(volume.root)
        inbox = inbox or volume.root / card_module.BOOK_INBOX
    if not annotations:
        raise ReaderError("No reader annotations with a passage or note were found")
    if out is None and inbox is None:
        raise ReaderError("Choose --inbox or --out for the Cross-Fire card")
    corpus = Path(getattr(args, "corpus", None) or DEFAULT_CORPUS)
    neighbors = Path(getattr(args, "neighbors", None) or DEFAULT_NEIGHBORS)
    database = getattr(args, "db", None)
    result = write_card(Path(inbox) if inbox else None, annotations, corpus=corpus,
                        neighbors=neighbors, replies=getattr(args, "replies", DEFAULT_REPLIES),
                        db=database if database is not None else DEFAULT_DB,
                        limit=getattr(args, "limit", DEFAULT_CANDIDATES),
                        dry_run=bool(getattr(args, "dry_run", False)),
                        out=Path(out) if out else None)
    return result
