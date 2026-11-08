"""The spoiler-safe story graph index (`story-graph.idx`, SGRF v1) the reader's CS-PW kernel reads.

Layout (little-endian, every section starts on a 512-byte sector and the file ends on one):

* header, 512 bytes: ``"SGRF"``, u16 version 1, u16 flags, u32 num_entities, u32 num_events,
  u32 num_aliases, u32 entities_offset (512), u32 events_offset, u32 aliases_offset, zeros.
* ``StoryEntityRecord`` x num_entities, 64 bytes each, sorted by ``(term_hash, book_path_hash)``:
  u64 term_hash, u64 book_path_hash (0 = any book), u32 first_cutoff, u32 event_start_idx,
  u16 event_count, u8 entity_kind, u8 flags, char en_name_or_role[32], char book_short[4].
* ``StoryEventRecord`` x num_events, 256 bytes each, contiguous per entity and ascending by cutoff:
  u32 cutoff, u16 chapter_or_page, u8 slot_hint, u8 salience_hint, char event_gloss[64],
  char target_or_loc[56], int8 vec[128].
* ``StoryAliasRecord`` x num_aliases, 24 bytes each, sorted by ``(alias_hash, reveal_cutoff, canonical_hash)``:
  u64 alias_hash, u64 canonical_hash, u32 reveal_cutoff, u32 reserved.

Hashes are FNV-1a 64 of UTF-8 (terms, aliases and the card book path such as ``/books/Books/x.epub``).
A cutoff is a reading position: EPUB ``((spine_index + 1) << 16) | min(0xFFFF, visible_codepoint >> 6)``,
TXT ``(1 << 16) | min(0xFFFF, byte_offset >> 10)``; 0 in a query means "no gate". The firmware's lookup,
mirrored by `query_story_graph`: a gate-passing alias (``cur == 0 or cur >= reveal_cutoff``) replaces the
term with its canonical hash; then the exact ``(term, book)`` record, else the wildcard ``(term, 0)`` one;
an entity with ``0 < cur < first_cutoff`` is not yet introduced; only events with ``cutoff <= cur`` show.

`refresh_story_graph` keeps `models/story-graph.idx` for card sync: a prebuilt index (the teacher
exporter's) is never touched; otherwise one is built from the card's Books, Inbox and Novels EPUBs
with the heuristic extractor, a bounded number of seconds per sync, reusing unchanged books' records.
"""
from __future__ import annotations

import hashlib
import html
import json
import re
import struct
import time
import zipfile
from collections import Counter
from pathlib import Path, PurePosixPath
from typing import Callable, Iterable

from .common import ReaderError, atomic_write, bounded_read
from .concept_index import (D_CONCEPT, MAX_EPUB_MEMBER_BYTES, MAX_INPUT_BYTES, _encoder_identity, fallback_encode,
                            fnv1a64)

STORY_GRAPH_NAME = "story-graph.idx"
# What the last card sync built: per-book stamps and titles, the encoder, and the SHA-256 of the index it
# wrote. An index whose SHA-256 differs was written by someone else (the teacher exporter) and is kept.
STATE_NAME = ".story-graph.json"
STATE_VERSION = 1
# Card shelves the sync-time builder reads, in the order it spends its time budget on.
STORY_SHELVES = ("books/Books", "books/Inbox", "books/Novels")
# No new book is started after this many seconds of a sync; one book costs up to a minute with the pure-Python
# model (an encode per event), so sync-time extraction keeps fewer entities and events than the default.
DEFAULT_BUDGET_SECONDS = 60.0
SYNC_MAX_ENTITIES = 24
SYNC_MAX_EVENTS = 6
MAX_INDEX_BYTES = 256 * 1024 * 1024

MAGIC = b"SGRF"
VERSION = 1
SECTOR = 512
HEADER = struct.Struct("<4sHHIIIIII")
ENTITY = struct.Struct("<QQIIHBB32s4s")
EVENT = struct.Struct(f"<IHBB64s56s{D_CONCEPT}b")
ALIAS = struct.Struct("<QQII")
assert (HEADER.size, ENTITY.size, EVENT.size, ALIAS.size) == (32, 64, 256, 24)

KIND_CHARACTER, KIND_LOCATION, KIND_FACTION, KIND_CONCEPT = 1, 2, 3, 4
SLOT_ROLE, SLOT_ORIGIN, SLOT_ARC, SLOT_CROSS_BOOK = 0, 1, 2, 3
MAX_EVENT_COUNT = 0xFFFF


def epub_cutoff(spine_index: int, codepoint_offset: int) -> int:
    """The firmware's EPUB reading position: spine + 1 in the high half, 64-codepoint buckets in the low."""
    if spine_index < 0 or spine_index >= 0xFFFF or codepoint_offset < 0:
        raise ReaderError(f"cutoff out of range: spine {spine_index}, offset {codepoint_offset}")
    return ((spine_index + 1) << 16) | min(0xFFFF, codepoint_offset >> 6)


def txt_cutoff(byte_offset: int) -> int:
    """The firmware's TXT reading position: spine field 1 (never the ungated 0), 1 KiB buckets."""
    if byte_offset < 0:
        raise ReaderError(f"cutoff out of range: byte offset {byte_offset}")
    return (1 << 16) | min(0xFFFF, byte_offset >> 10)


def _align(offset: int) -> int:
    return -(-offset // SECTOR) * SECTOR


def _fixed(text: str, size: int) -> bytes:
    """UTF-8 cut at a codepoint boundary to at most size - 1 bytes; struct pads with NULs."""
    data = (text or "").encode("utf-8")[:size - 1]
    return data.decode("utf-8", "ignore").encode("utf-8")


def _string(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def _hash(value: str | None) -> int:
    return fnv1a64(value.encode("utf-8")) if value else 0


def _term_hash(record: dict, text_key: str, hash_key: str) -> int:
    if record.get(hash_key) is not None:
        return int(record[hash_key])
    if not record.get(text_key):
        raise ReaderError(f"story graph record needs {text_key!r} or {hash_key!r}")
    return _hash(record[text_key])


def _vector(values) -> list[int]:
    vector = [0] * D_CONCEPT if values is None else [int(value) for value in values]
    if len(vector) != D_CONCEPT or any(value < -128 or value > 127 for value in vector):
        raise ReaderError(f"story event vectors must be {D_CONCEPT} int8 values")
    return vector


def build_story_graph(entities: Iterable[dict], aliases: Iterable[dict] = ()) -> bytes:
    """SGRF bytes for `entities` and `aliases`.

    An entity is ``{"term" | "term_hash", "book_path" | "book_path_hash" (absent/0 = any book), "kind",
    "flags", "name", "book_short", "first_cutoff" (default: its earliest event), "events": [{"cutoff",
    "chapter_or_page", "slot_hint", "salience_hint", "gloss", "target", "vec"}]}``; an alias is
    ``{"alias" | "alias_hash", "canonical" | "canonical_hash", "reveal_cutoff"}``. `read_story_graph`
    returns the same shapes, so an index can be read, edited and rebuilt.
    """
    rows = []
    for entity in entities:
        term_hash = _term_hash(entity, "term", "term_hash")
        book_hash = (int(entity["book_path_hash"]) if entity.get("book_path_hash") is not None
                     else _hash(entity.get("book_path")))
        events = sorted(entity.get("events") or (), key=lambda event: (int(event["cutoff"]),
                                                                        int(event.get("chapter_or_page", 0))))
        if len(events) > MAX_EVENT_COUNT:
            raise ReaderError(f"{entity.get('name') or term_hash}: more than {MAX_EVENT_COUNT} events")
        first_cutoff = entity.get("first_cutoff")
        if first_cutoff is None:
            first_cutoff = int(events[0]["cutoff"]) if events else 0
        rows.append((term_hash, book_hash, int(first_cutoff), entity, events))
    rows.sort(key=lambda row: (row[0], row[1]))
    for previous, current in zip(rows, rows[1:]):
        if (previous[0], previous[1]) == (current[0], current[1]):
            raise ReaderError(f"duplicate story graph entity {current[3].get('name') or current[3].get('term')!r}")
    alias_rows = sorted({(_term_hash(alias, "alias", "alias_hash"), int(alias.get("reveal_cutoff", 0)),
                          _term_hash(alias, "canonical", "canonical_hash")) for alias in aliases})

    entity_bytes, event_bytes = [], []
    for term_hash, book_hash, first_cutoff, entity, events in rows:
        entity_bytes.append(ENTITY.pack(
            term_hash, book_hash, first_cutoff & 0xFFFFFFFF, len(event_bytes), len(events),
            int(entity.get("kind", KIND_CONCEPT)), int(entity.get("flags", 0)),
            _fixed(entity.get("name") or entity.get("term") or "", 32), _fixed(entity.get("book_short", ""), 4)))
        for event in events:
            event_bytes.append(EVENT.pack(
                int(event["cutoff"]) & 0xFFFFFFFF, int(event.get("chapter_or_page", 0)) & 0xFFFF,
                int(event.get("slot_hint", SLOT_ARC)), max(0, min(255, int(event.get("salience_hint", 0)))),
                _fixed(event.get("gloss", ""), 64), _fixed(event.get("target", ""), 56),
                *_vector(event.get("vec"))))
    alias_bytes = [ALIAS.pack(alias_hash, canonical_hash, reveal & 0xFFFFFFFF, 0)
                   for alias_hash, reveal, canonical_hash in alias_rows]

    entities_offset = SECTOR
    events_offset = _align(entities_offset + ENTITY.size * len(entity_bytes))
    aliases_offset = _align(events_offset + EVENT.size * len(event_bytes))
    total = _align(aliases_offset + ALIAS.size * len(alias_bytes))
    data = bytearray(total)
    HEADER.pack_into(data, 0, MAGIC, VERSION, 0, len(entity_bytes), len(event_bytes), len(alias_bytes),
                     entities_offset, events_offset, aliases_offset)
    for offset, chunk in ((entities_offset, b"".join(entity_bytes)), (events_offset, b"".join(event_bytes)),
                          (aliases_offset, b"".join(alias_bytes))):
        data[offset:offset + len(chunk)] = chunk
    return bytes(data)


def build_story_graph_index(entities: Iterable[dict], aliases: Iterable[dict], out_path: Path) -> dict:
    """Write `build_story_graph(entities, aliases)` to `out_path` (atomically, only when the bytes change)."""
    data = build_story_graph(entities, aliases)
    problems = verify_story_graph(data)
    if problems:
        raise ReaderError(f"story graph failed verification: {problems[0]}")
    out_path = Path(out_path)
    current = out_path.read_bytes() if out_path.is_file() and not out_path.is_symlink() else None
    updated = current != data
    if updated:
        atomic_write(out_path, data)
    header = _header(data)
    return {"path": str(out_path), "bytes": len(data), "entities": header["num_entities"],
            "events": header["num_events"], "aliases": header["num_aliases"], "updated": updated,
            "sha256": hashlib.sha256(data).hexdigest()}


def _header(data: bytes) -> dict:
    if len(data) < SECTOR:
        raise ReaderError("story-graph.idx is shorter than its 512-byte header")
    (magic, version, flags, num_entities, num_events, num_aliases,
     entities_offset, events_offset, aliases_offset) = HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION:
        raise ReaderError("story-graph.idx is not a version-1 SGRF index")
    header = {"flags": flags, "num_entities": num_entities, "num_events": num_events, "num_aliases": num_aliases,
              "entities_offset": entities_offset, "events_offset": events_offset, "aliases_offset": aliases_offset}
    for name, offset, size in (("entities", entities_offset, ENTITY.size * num_entities),
                               ("events", events_offset, EVENT.size * num_events),
                               ("aliases", aliases_offset, ALIAS.size * num_aliases)):
        if offset % SECTOR or offset < SECTOR or offset + size > len(data):
            raise ReaderError(f"story-graph.idx {name} section at {offset} (+{size}) is misaligned or truncated")
    return header


def _entity_at(data: bytes, header: dict, index: int) -> dict:
    (term_hash, book_hash, first_cutoff, start, count, kind, flags, name, book_short) = ENTITY.unpack_from(
        data, header["entities_offset"] + index * ENTITY.size)
    return {"term_hash": term_hash, "book_path_hash": book_hash, "first_cutoff": first_cutoff,
            "event_start_idx": start, "event_count": count, "kind": kind, "flags": flags,
            "name": _string(name), "book_short": _string(book_short)}


def _event_at(data: bytes, header: dict, index: int) -> dict:
    cutoff, chapter, slot, salience, gloss, target, *vec = EVENT.unpack_from(
        data, header["events_offset"] + index * EVENT.size)
    return {"cutoff": cutoff, "chapter_or_page": chapter, "slot_hint": slot, "salience_hint": salience,
            "gloss": _string(gloss), "target": _string(target), "vec": vec}


def _alias_at(data: bytes, header: dict, index: int) -> dict:
    alias_hash, canonical_hash, reveal, _ = ALIAS.unpack_from(data, header["aliases_offset"] + index * ALIAS.size)
    return {"alias_hash": alias_hash, "canonical_hash": canonical_hash, "reveal_cutoff": reveal}


def _events_of(data: bytes, header: dict, entity: dict) -> list[dict]:
    start, count = entity["event_start_idx"], entity["event_count"]
    if start + count > header["num_events"]:
        raise ReaderError(f"story-graph.idx entity events {start}+{count} run past {header['num_events']}")
    return [_event_at(data, header, index) for index in range(start, start + count)]


def read_story_graph(data: bytes) -> dict:
    """Every entity (with its events) and alias in the index, in file order."""
    header = _header(data)
    entities = []
    for index in range(header["num_entities"]):
        entity = _entity_at(data, header, index)
        entity["events"] = _events_of(data, header, entity)
        entities.append(entity)
    aliases = [_alias_at(data, header, index) for index in range(header["num_aliases"])]
    return {**header, "entities": entities, "aliases": aliases}


def verify_story_graph(data: bytes) -> list[str]:
    """Problems that would make the firmware's binary search or spoiler gates wrong; empty when sound."""
    try:
        graph = read_story_graph(data)
    except ReaderError as exc:
        return [str(exc)]
    problems = []
    if len(data) % SECTOR:
        problems.append(f"file size {len(data)} is not a multiple of {SECTOR}")
    if graph["entities_offset"] != SECTOR:
        problems.append(f"entities start at {graph['entities_offset']}, not {SECTOR}")
    if not (graph["entities_offset"] + ENTITY.size * graph["num_entities"] <= graph["events_offset"]
            and graph["events_offset"] + EVENT.size * graph["num_events"] <= graph["aliases_offset"]):
        problems.append("sections overlap or are out of order")
    keys = [(entity["term_hash"], entity["book_path_hash"]) for entity in graph["entities"]]
    if any(previous >= current for previous, current in zip(keys, keys[1:])):
        problems.append("entities are not strictly sorted by (term_hash, book_path_hash)")
    covered = [False] * graph["num_events"]
    for entity in graph["entities"]:
        label = entity["name"] or f"{entity['term_hash']:016x}"
        if entity["kind"] not in (KIND_CHARACTER, KIND_LOCATION, KIND_FACTION, KIND_CONCEPT):
            problems.append(f"{label}: unknown entity kind {entity['kind']}")
        events = entity["events"]
        for index in range(entity["event_start_idx"], entity["event_start_idx"] + entity["event_count"]):
            if covered[index]:
                problems.append(f"{label}: event {index} is shared with another entity")
            covered[index] = True
        cutoffs = [event["cutoff"] for event in events]
        if cutoffs != sorted(cutoffs):
            problems.append(f"{label}: events are not ascending by cutoff")
        pages = [event["chapter_or_page"] for event in events]
        if pages != sorted(pages):
            problems.append(f"{label}: chapter_or_page is not monotone (the CfC decay reads it as pages)")
        if any(event["slot_hint"] > SLOT_CROSS_BOOK for event in events):
            problems.append(f"{label}: slot_hint above {SLOT_CROSS_BOOK}")
        if entity["first_cutoff"] and cutoffs and cutoffs[0] < entity["first_cutoff"]:
            problems.append(f"{label}: an event (cutoff {cutoffs[0]:#x}) precedes the spoiler gate "
                            f"(first_cutoff {entity['first_cutoff']:#x})")
    if not all(covered):
        problems.append("some events belong to no entity")
    alias_keys = [(alias["alias_hash"], alias["reveal_cutoff"], alias["canonical_hash"]) for alias in graph["aliases"]]
    if alias_keys != sorted(alias_keys) or len(set(alias_keys)) != len(alias_keys):
        problems.append("aliases are not sorted by (alias_hash, reveal_cutoff, canonical_hash) without duplicates")
    terms = {entity["term_hash"] for entity in graph["entities"]}
    for alias in graph["aliases"]:
        if alias["canonical_hash"] not in terms:
            problems.append(f"alias {alias['alias_hash']:016x} names a canonical entity the index lacks")
    return problems


def _find_entity(data: bytes, header: dict, term_hash: int, book_hash: int) -> int | None:
    low, high = 0, header["num_entities"]
    key = (term_hash, book_hash)
    while low < high:
        middle = (low + high) // 2
        found = struct.unpack_from("<QQ", data, header["entities_offset"] + middle * ENTITY.size)
        if found < key:
            low = middle + 1
        elif found > key:
            high = middle
        else:
            return middle
    return None


def _resolve_alias(data: bytes, header: dict, term_hash: int, current_cutoff: int) -> tuple[int | None, bool]:
    """(canonical hash of the first gate-passing alias record, whether any record for the term was gated)."""
    low, high = 0, header["num_aliases"]
    while low < high:  # lower bound of alias_hash
        middle = (low + high) // 2
        if struct.unpack_from("<Q", data, header["aliases_offset"] + middle * ALIAS.size)[0] < term_hash:
            low = middle + 1
        else:
            high = middle
    gated = False
    for index in range(low, header["num_aliases"]):
        alias = _alias_at(data, header, index)
        if alias["alias_hash"] != term_hash:
            break
        if current_cutoff == 0 or current_cutoff >= alias["reveal_cutoff"]:
            return alias["canonical_hash"], gated
        gated = True
    return None, gated


def query_story_graph(index_path: Path | bytes, term: str, book_path: str | None, current_cutoff: int) -> dict:
    """Look `term` up as the firmware does at reading position `current_cutoff` (0 = no gate).

    Returns ``{"status": "found" | "not_yet_introduced" | "not_found", "term_hash", "lookup_hash",
    "alias": "revealed" | "not_revealed" | None, "entity": {...} | None, "events": [...]}``; `events` holds
    only ``cutoff <= current_cutoff`` rows and is empty unless the status is "found".
    """
    data = index_path if isinstance(index_path, (bytes, bytearray)) else bounded_read(Path(index_path),
                                                                                       MAX_INDEX_BYTES)
    header = _header(data)
    term_hash = _hash(term)
    canonical, gated = _resolve_alias(data, header, term_hash, current_cutoff)
    lookup = canonical if canonical is not None else term_hash
    alias = "revealed" if canonical is not None else ("not_revealed" if gated else None)
    result = {"status": "not_found", "term_hash": term_hash, "lookup_hash": lookup, "alias": alias,
              "entity": None, "events": []}
    index = _find_entity(data, header, lookup, _hash(book_path))
    if index is None and book_path:
        index = _find_entity(data, header, lookup, 0)
    if index is None:
        return result
    entity = _entity_at(data, header, index)
    result["entity"] = entity
    if 0 < current_cutoff < entity["first_cutoff"]:
        result["status"] = "not_yet_introduced"
        return result
    result["status"] = "found"
    result["events"] = [event for event in _events_of(data, header, entity)
                        if current_cutoff == 0 or event["cutoff"] <= current_cutoff]
    return result


# --- EPUB text with firmware-compatible offsets ---------------------------------------------------------

_COMMENT = re.compile(r"<!--.*?-->", re.DOTALL)
_HIDDEN = re.compile(r"<(script|style|head)\b.*?</\1\s*>", re.IGNORECASE | re.DOTALL)
_BODY = re.compile(r"<body\b[^>]*>(.*)</body\s*>", re.IGNORECASE | re.DOTALL)
_TAG = re.compile(r"<[^>]*>")
_CDATA = re.compile(r"<!\[CDATA\[(.*?)\]\]>", re.DOTALL)


def visible_text(markup: str) -> str:
    """The body character data the firmware's chapter parser counts codepoints over: tags removed (not
    replaced by spaces), entities decoded, whitespace kept, head/script/style/comments dropped."""
    markup = _COMMENT.sub("", markup)
    body = _BODY.search(markup)
    markup = _HIDDEN.sub("", body.group(1) if body else markup)
    markup = _CDATA.sub(lambda found: html.escape(found.group(1), quote=False), markup)
    return html.unescape(_TAG.sub("", markup))


def epub_spine_texts(path: Path) -> tuple[str, list[str]]:
    """(dc:title or file stem, visible text of every spine item in spine order; "" for unreadable items)."""
    path = Path(path)
    title, texts = "", []
    try:
        with zipfile.ZipFile(path) as archive:
            def member(name: str) -> str:
                info = archive.getinfo(name)
                if info.file_size > MAX_EPUB_MEMBER_BYTES:
                    raise ReaderError(f"{name} is too large")
                return archive.read(info).decode("utf-8", "replace")

            found = re.search(r"full-path\s*=\s*[\"']([^\"']+)", member("META-INF/container.xml"))
            if found is None:
                raise ReaderError(f"{path.name}: no rootfile")
            opf_name = html.unescape(found.group(1))
            opf = member(opf_name)
            found = re.search(r"<dc:title\b[^>]*>(.*?)</dc:title>", opf, re.IGNORECASE | re.DOTALL)
            title = re.sub(r"\s+", " ", html.unescape(_TAG.sub("", found.group(1)))).strip() if found else ""
            hrefs = {}
            for item in re.finditer(r"<item\b[^>]*>", opf, re.IGNORECASE):
                attributes = dict(re.findall(r"([\w:-]+)\s*=\s*[\"']([^\"']*)[\"']", item.group(0)))
                if "id" in attributes and "href" in attributes:
                    hrefs[attributes["id"]] = html.unescape(attributes["href"])
            base = PurePosixPath(opf_name).parent
            for reference in re.finditer(r"<itemref\b[^>]*\bidref\s*=\s*[\"']([^\"']+)", opf, re.IGNORECASE):
                href = hrefs.get(reference.group(1))
                text = ""
                if href is not None:
                    try:
                        text = visible_text(member((base / href.split("#", 1)[0]).as_posix()))
                    except (KeyError, ReaderError):
                        text = ""
                texts.append(text)
    except (OSError, zipfile.BadZipFile, KeyError, RuntimeError, ValueError) as exc:
        raise ReaderError(f"{path.name}: unreadable EPUB: {exc}") from exc
    return title or path.stem, texts


# --- Heuristic entity extraction --------------------------------------------------------------------------

_CJK = re.compile(r"[\u3400-\u9fff]")
_SURNAMES = ("王李张刘陈杨黄赵吴周徐孙马朱胡郭何高林罗郑梁谢宋唐许韩冯邓曹彭曾肖田董袁潘蒋蔡余杜叶程苏魏吕丁"
             "任沈姚卢姜崔钟谭陆汪范金石廖贾夏韦付方白邹孟熊秦邱江尹薛闫段雷侯龙史陶黎贺顾毛郝龚邵万钱严覃武"
             "戴莫孔汤萧柳慕楚燕洛云风岳牧古宁秋凌君霍乔聂穆")
_COMPOUND_SURNAMES = ("诸葛", "欧阳", "上官", "司马", "司徒", "东方", "南宫", "令狐", "独孤", "皇甫", "慕容", "纳兰")
# Characters that end, rather than continue, a Chinese name: particles, verbs of speech, pronouns.
_CJK_STOP = set("的了是在着也和就都把被说道笑看对向与从这那一不人们个上下中来去到过又还要会能有没你我他她它之而"
                "所以得地么吗呢吧啊呀哦嗯啦么此其为于以及并很太更最再已经将让给跟同而且却只但便都")
_CJK_ACTION = set("道说笑问点摇叹喝怒")
_CJK_LEAD = set("对向和与跟让被将把见找")
_CJK_COMMON = {"高兴", "方面", "方向", "方法", "金色", "白色", "黄色", "万一", "于是", "向前", "江湖", "石头",
               "龙族", "风声", "云层", "马上", "严重", "任务", "任何", "史上", "成为", "周围", "时候", "林中",
               "白天", "黄金", "高手", "高级", "高空", "金钱", "白衣", "毛病", "龙气", "孔雀", "钱财", "罗盘",
               "宋代", "唐代", "秦国", "楚国", "魏国", "韩国", "赵国", "燕国", "明白", "古老", "古代", "宁可",
               "秋天", "霍然", "武器", "武功", "武道", "雷电", "雷霆", "温和", "夏天", "田地", "程度", "苏醒",
               "陆地", "江水", "汤水", "柳树", "叶子", "常常", "连忙", "安静", "方才", "高大", "白发",
               "张开", "张嘴", "胡子", "胡须", "石块", "石壁", "石柱", "金砖", "金属", "白种", "毛发", "龙卷",
               "风暴", "云雾", "雷声", "许多", "钱袋", "风格", "严格", "风度", "段话", "古典"}

_EN_NAME = re.compile(r"(?<![\w’'])[A-Z][a-z’']+(?:[ \u00a0](?:of |de |von |van |al-|ibn |ben )?[A-Z][a-z’']+){0,2}"
                      r"(?![\w’'])")
_EN_STOP = {"The", "This", "That", "These", "Those", "There", "Then", "Thus", "They", "Their", "He", "She", "It",
            "His", "Her", "Its", "We", "Our", "You", "Your", "I", "In", "On", "At", "As", "And", "But", "Or",
            "If", "When", "While", "What", "Which", "Who", "Whom", "Why", "How", "Where", "For", "From", "With",
            "By", "To", "Of", "A", "An", "All", "Some", "Many", "Most", "No", "Not", "Yet", "So", "Such",
            "After", "Before", "Although", "Though", "Because", "Since", "Chapter", "Part", "Book", "Contents",
            "Introduction", "Notes", "Index", "Bibliography", "One", "Two", "Three", "First", "Second", "Mr",
            "Mrs", "Dr", "January", "February", "March", "April", "May", "June", "July", "August", "September",
            "October", "November", "December", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
            "Saturday", "Sunday", "Here", "Now", "Even", "Only", "Also", "Every", "Each", "Other", "Another"}
_EN_TITLES = re.compile(r"(?:Mr|Mrs|Ms|Dr|Rabbi|Father|Saint|St|King|Queen|Lord|Lady|Sir|Pope|Cardinal|Brother|"
                        r"Sister|Prince|Princess|Master|General|Captain|Count|Duke|Emperor)\.?\s$")
_EN_VERBS = re.compile(r"^\s(?:said|says|wrote|writes|taught|teaches|asked|replied|answered|believed|argued|"
                       r"told|thought|cried|smiled|laughed|whispered|shouted|died|was born)\b")
_EN_PLACE_LEAD = re.compile(r"(?:\bin|\bat|\bfrom|\bnear|\bto|\btoward|\binto)\s$")
_SENTENCE_END = re.compile(r"[.!?。！？…\n]")


def _discover_chinese(text: str, min_mentions: int) -> dict[str, int]:
    pattern = "|".join(_COMPOUND_SURNAMES) + "|[" + "".join(sorted(set(_SURNAMES))) + "]"
    two, three, acted = Counter(), Counter(), Counter()
    for found in re.finditer(f"(?=((?:{pattern})[\u4e00-\u9fff]{{1,2}}))", text):
        candidate = found.group(1)
        start = found.start()
        surname = 2 if candidate[:2] in _COMPOUND_SURNAMES and len(candidate) >= 3 else 1
        given = candidate[surname:]
        if not given or given[0] in _CJK_STOP:
            continue
        short = candidate[:surname + 1]
        two[short] += 1
        follow = text[start + len(short):start + len(short) + 1]
        lead = text[start - 1:start] if start else ""
        if follow in _CJK_ACTION or lead in _CJK_LEAD:
            acted[short] += 1
        if len(given) == 2 and given[1] not in _CJK_STOP:
            three[candidate[:surname + 2]] += 1
            follow = text[start + surname + 2:start + surname + 3]
            if follow in _CJK_ACTION or lead in _CJK_LEAD:
                acted[candidate[:surname + 2]] += 1
    extension: dict[str, tuple[int, str]] = {}
    for name, count in three.items():
        extension[name[:-1]] = max(extension.get(name[:-1], (0, "")), (count, name))
    names: dict[str, int] = {}
    for short, count in two.items():
        if count < min_mentions or short in _CJK_COMMON:
            continue
        best_count, best = extension.get(short, (0, ""))
        name, name_count = (best, best_count) if best_count >= 0.6 * count else (short, count)
        if name_count >= min_mentions and acted[name] >= max(2, name_count // 12):
            names[name] = name_count
    # "付雷恩" is "付" glued to the name "雷恩": a name that contains a more frequent one is not a name.
    return {name: count for name, count in names.items()
            if not any(other != name and other in name and names[other] >= count for other in names)}


def _discover_english(text: str, min_mentions: int) -> dict[str, tuple[int, int]]:
    counts, kinds = Counter(), {}
    lead_place, lead_person = Counter(), Counter()
    for found in _EN_NAME.finditer(text):
        name = re.sub(r"[’']s$", "", found.group(0).replace("\u00a0", " "))
        words = name.split(" ")
        while words and words[0] in _EN_STOP:
            words.pop(0)
        if not words or (len(words) == 1 and len(words[0]) < 3):
            continue
        name = " ".join(words)
        before = text[max(0, found.start() - 12):found.start()]
        stripped = before.rstrip()
        if (not stripped or stripped[-1] in ".!?\"“”‘’:") and len(words) == 1:
            continue  # a capitalised sentence opener is not evidence of a name
        counts[name] += 1
        if _EN_PLACE_LEAD.search(before):
            lead_place[name] += 1
        if (_EN_TITLES.search(before) or _EN_VERBS.match(text[found.end():found.end() + 16])
                or (len(words) > 1 and _EN_TITLES.match(words[0] + " "))):
            lead_person[name] += 1
    names = {name: count for name, count in counts.items() if count >= min_mentions}
    for name in list(names):
        if " " in name:
            continue
        dominated = [other for other in names if other != name and name in other.split(" ")
                     and names[other] >= 0.6 * names[name]]
        if dominated:
            del names[name]
    for name, count in names.items():
        if lead_person[name] * 5 >= count:
            kinds[name] = KIND_CHARACTER
        elif lead_place[name] * 5 >= count * 2:
            kinds[name] = KIND_LOCATION
        else:
            kinds[name] = KIND_CONCEPT
    return {name: (count, kinds[name]) for name, count in names.items()}


def _snippet(text: str, start: int, end: int) -> tuple[str, int]:
    """(gloss around text[start:end] cut to 63 UTF-8 bytes, offset just past it); never reads past the mention's
    sentence, so the event's cutoff (taken at that offset) covers every character it shows."""
    sentence = start
    while sentence > 0 and not _SENTENCE_END.match(text[sentence - 1]) and start - sentence < 80:
        sentence -= 1
    finish = end
    while finish < len(text) and not _SENTENCE_END.match(text[finish]) and finish - end < 80:
        finish += 1
    if finish < len(text) and text[finish] != "\n":
        finish += 1
    collapse = lambda value: re.sub(r"\s+", " ", value).strip()  # noqa: E731
    left = sentence
    while len(collapse(text[left:end]).encode("utf-8")) > 63 and left < start:
        left += 1
    right = end
    while right < finish and len(collapse(text[left:right + 1]).encode("utf-8")) <= 63:
        right += 1
    return collapse(text[left:right]), right


def extract_book_story_entities(epub_path: Path, book_path: str, *, terms: Iterable[dict] | None = None,
                                aliases: Iterable[dict] = (), encode: Callable[[bytes], list[int]] | None = None,
                                max_entities: int = 32, max_events: int = 8, min_mentions: int = 4,
                                discover_chars: int = 2_000_000) -> dict:
    """Spoiler-gated entities of one EPUB, ready for `build_story_graph`.

    `book_path` is the card path (``/books/Books/x.epub``) the firmware hashes. Entities come from `terms`
    (``{"term", "kind", "name"}``, e.g. a teacher's dossier list) or, without them, from heuristics:
    surname-led Chinese names that act or speak, and capitalised English names seen mid-sentence. Each
    entity gets one event per chapter it appears in (the origin plus the `max_events - 1` chapters that
    mention it most): the first mention's sentence, cut so the event's cutoff (taken at the end of the
    gloss) covers every character shown, the nearest earlier co-occurring entity, and a 128-d vector of
    the text that ends there (`encode`, default the model-free fallback encoder). `aliases`
    (``{"alias", "canonical", "reveal_cutoff"?}``) default their reveal to the alias's first appearance.
    Returns ``{"title", "book_path", "entities", "aliases"}``.
    """
    encode = encode or fallback_encode
    title, texts = epub_spine_texts(Path(epub_path))
    if terms is not None:
        chosen = {}
        for term in terms:
            surface = term["term"] if isinstance(term, dict) else str(term)
            chosen[surface] = {"kind": int(term.get("kind", KIND_CONCEPT)) if isinstance(term, dict) else KIND_CONCEPT,
                               "name": (term.get("name") if isinstance(term, dict) else None) or surface}
    else:
        sample = "".join(texts)[:discover_chars]
        cjk = len(_CJK.findall(sample[:20000])) * 4 > len(sample[:20000])
        found = ({name: (count, KIND_CHARACTER) for name, count in _discover_chinese(sample, min_mentions).items()}
                 if cjk else _discover_english(sample, min_mentions))
        ranked = sorted(found.items(), key=lambda item: (-item[1][0], item[0]))[:max_entities]
        chosen = {name: {"kind": kind, "name": name} for name, (_, kind) in ranked}

    # Every mention of every chosen term, per spine item; co-occurrence targets come from these.
    mentions: dict[str, dict[int, list[int]]] = {}
    for surface in chosen:
        per_spine = {}
        for spine, text in enumerate(texts):
            positions, at = [], text.find(surface)
            while at >= 0:
                positions.append(at)
                at = text.find(surface, at + len(surface))
            if positions:
                per_spine[spine] = positions
        if per_spine:
            mentions[surface] = per_spine

    entities = []
    for surface, per_spine in mentions.items():
        spines = sorted(per_spine)
        keep = {spines[0]} | {spine for spine in sorted(spines[1:], key=lambda s: (-len(per_spine[s]), s))
                              [:max(0, max_events - 1)]}
        most = max(spines, key=lambda s: (len(per_spine[s]), -s))
        events = []
        for spine in sorted(keep):
            text, first = texts[spine], per_spine[spine][0]
            gloss, end = _snippet(text, first, first + len(surface))
            target, nearest = "", -1
            for other, other_spines in mentions.items():
                if other == surface or other in surface or surface in other:
                    continue
                earlier = [position for position in other_spines.get(spine, ()) if position + len(other) <= end]
                if earlier and earlier[-1] > nearest and end - earlier[-1] <= 4000:
                    target, nearest = chosen[other]["name"], earlier[-1]
            context = re.sub(r"\s+", " ", text[max(0, end - 600):end]).strip().encode("utf-8")
            context = context[-MAX_INPUT_BYTES:].decode("utf-8", "ignore").encode("utf-8")
            # Rounded up to the next 64-codepoint bucket so the gloss is never ahead of the reader.
            events.append({"cutoff": epub_cutoff(spine, end + 63), "chapter_or_page": min(0xFFFF, spine + 1),
                           "slot_hint": SLOT_ORIGIN if spine == spines[0] else (SLOT_ROLE if spine == most else SLOT_ARC),
                           "salience_hint": min(255, 16 * len(per_spine[spine])),
                           "gloss": gloss, "target": target, "vec": encode(context)})
        entities.append({"term": surface, "book_path": book_path, "kind": chosen[surface]["kind"], "flags": 0,
                         "name": chosen[surface]["name"], "book_short": "", "first_cutoff": events[0]["cutoff"],
                         "events": events})

    resolved = []
    for alias in aliases:
        reveal = alias.get("reveal_cutoff")
        if reveal is None:
            for spine, text in enumerate(texts):
                at = text.find(alias["alias"])
                if at >= 0:
                    reveal = epub_cutoff(spine, at + len(alias["alias"]))
                    break
        if reveal is not None:
            resolved.append({"alias": alias["alias"], "canonical": alias["canonical"], "reveal_cutoff": reveal})
    return {"title": title, "book_path": book_path, "entities": entities, "aliases": resolved}


def _jsonl(path: Path) -> list[dict]:
    rows = []
    for number, line in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), start=1):
        if line.strip():
            try:
                row = json.loads(line)
            except ValueError as exc:
                raise ReaderError(f"{Path(path).name}:{number}: not JSON: {exc}") from exc
            if not isinstance(row, dict):
                raise ReaderError(f"{Path(path).name}:{number}: not a JSON object")
            rows.append(row)
    return rows


def load_teacher_timelines(timelines_path: Path, aliases_path: Path | None = None,
                           encode: Callable[[bytes], list[int]] | None = None) -> tuple[list[dict], list[dict]]:
    """(entities, aliases) for `build_story_graph` from the teacher exporter's JSONL.

    A timeline row is ``{"term", "book_path", "entity_kind", "en_name_or_role", "first_cutoff", "events": [{"cutoff",
    "chapter_or_page", "slot_hint", "salience_hint", "event_gloss", "target_or_loc", "scene_text"}], "aliases"}``;
    each event's vector is `encode(scene_text)` (default the fallback encoder; pass the PLHN model's). Substack
    rows (``"source": "substack"`` or a ``/substack/`` path) are not card books, so they become wildcard records
    whose events are always visible cross-book echoes. Rows for the same record are merged; aliases (from the
    rows and the optional aliases JSONL ``{"alias", "canonical", "reveal_cutoff"}``) whose canonical term has no
    entity are dropped.
    """
    encode = encode or fallback_encode
    merged: dict[tuple[str, str | None], dict] = {}
    aliases: list[dict] = []
    for row in _jsonl(timelines_path):
        book_path = row.get("book_path") or None
        wildcard = row.get("source") == "substack" or (book_path or "").startswith("/substack/")
        key = (row["term"], None if wildcard else book_path)
        entity = merged.setdefault(key, {"term": row["term"], "book_path": key[1],
                                         "kind": int(row.get("entity_kind", KIND_CONCEPT)),
                                         "name": row.get("en_name_or_role") or row["term"], "events": []})
        if not wildcard and row.get("first_cutoff") is not None:
            entity["first_cutoff"] = min(int(row["first_cutoff"]), entity.get("first_cutoff", 0xFFFFFFFF))
        if wildcard:
            entity["first_cutoff"] = 0
        for item in row.get("events") or ():
            scene = (item.get("scene_text") or "").encode("utf-8")[:MAX_INPUT_BYTES]
            entity["events"].append({
                "cutoff": 0 if wildcard else int(item["cutoff"]),
                "chapter_or_page": int(item.get("chapter_or_page", 0)),
                "slot_hint": SLOT_CROSS_BOOK if wildcard else int(item.get("slot_hint", SLOT_ARC)),
                "salience_hint": int(item.get("salience_hint", 0)),
                "gloss": item.get("event_gloss", ""), "target": item.get("target_or_loc", ""),
                "vec": encode(scene.decode("utf-8", "ignore").encode("utf-8")) if scene else [0] * D_CONCEPT})
        for alias in row.get("aliases") or ():
            if isinstance(alias, str):
                alias = {"alias": alias}
            aliases.append({"alias": alias["alias"], "canonical": alias.get("canonical") or row["term"],
                            "reveal_cutoff": 0 if wildcard else int(alias.get("reveal_cutoff",
                                                                              row.get("first_cutoff") or 0))})
    if aliases_path is not None:
        for alias in _jsonl(aliases_path):
            wildcard = (alias.get("book_path") or "").startswith("/substack/")
            aliases.append({"alias": alias["alias"], "canonical": alias["canonical"],
                            "reveal_cutoff": 0 if wildcard else int(alias.get("reveal_cutoff", 0))})
    entities = list(merged.values())
    for entity in entities:
        # Merged wildcard rows share cutoff 0, so their page numbers are renumbered to stay monotone.
        entity["events"].sort(key=lambda item: (item["cutoff"], item["chapter_or_page"]))
        pages = [item["chapter_or_page"] for item in entity["events"]]
        if pages != sorted(pages):
            for number, item in enumerate(entity["events"], start=1):
                item["chapter_or_page"] = number
    terms = {entity["term"] for entity in entities}
    return entities, [alias for alias in aliases if alias["canonical"] in terms]


def cross_book_entities(books: list[dict]) -> list[dict]:
    """Wildcard (book_path_hash 0) entries for terms found in two or more books: one always-visible
    cross-book event per book, citing its title and chapter with that book's origin gloss."""
    by_term: dict[int, list[tuple[dict, dict]]] = {}
    for book in books:
        for entity in book["entities"]:
            if entity.get("events"):
                by_term.setdefault(_term_hash(entity, "term", "term_hash"), []).append((book, entity))
    wildcard = []
    for term_hash, found in sorted(by_term.items()):
        if len({book["book_path"] for book, _ in found}) < 2:
            continue
        events = []
        for number, (book, entity) in enumerate(sorted(found, key=lambda pair: pair[0]["book_path"]), start=1):
            origin = entity["events"][0]
            events.append({"cutoff": 0, "chapter_or_page": number, "slot_hint": SLOT_CROSS_BOOK,
                           "salience_hint": origin["salience_hint"], "gloss": origin["gloss"],
                           "target": f"{book['title']} Ch.{origin['chapter_or_page']}", "vec": origin["vec"]})
        first = found[0][1]
        wildcard.append({"term_hash": term_hash, "book_path_hash": 0, "kind": first["kind"], "flags": 0,
                         "name": first["name"], "book_short": "", "first_cutoff": 0, "events": events})
    return wildcard


# --- Card-sync maintenance -----------------------------------------------------------------------------------

def story_books(card_root: Path) -> list[tuple[str, Path]]:
    """("/books/Books/x.epub", path) for every visible EPUB on the story shelves, Books first, then Inbox, Novels."""
    books = []
    for shelf in STORY_SHELVES:
        root = Path(card_root) / shelf
        if root.is_symlink() or not root.is_dir():
            continue
        found, pending = [], [root]
        while pending:
            folder = pending.pop()
            for entry in folder.iterdir():
                if entry.name.startswith(".") or entry.is_symlink():
                    continue
                if entry.is_dir():
                    pending.append(entry)
                elif entry.is_file() and entry.suffix.lower() == ".epub" and not entry.name.endswith(".pre-hooks.epub"):
                    found.append(("/" + entry.relative_to(card_root).as_posix(), entry))
        books += sorted(found)
    return books


def _load_state(models_dir: Path) -> dict:
    try:
        state = json.loads((models_dir / STATE_NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    if not isinstance(state, dict) or state.get("version") != STATE_VERSION or not isinstance(state.get("books"), dict):
        return {}
    return state


def refresh_story_graph(models_dir: Path, card_root: Path, *, budget_seconds: float = DEFAULT_BUDGET_SECONDS,
                        clock: Callable[[], float] = time.monotonic) -> dict:
    """Keep `models_dir/story-graph.idx` current for the card's Books, Inbox and Novels EPUBs.

    A story-graph.idx this function did not write (the teacher exporter's) is left exactly as it is. Otherwise
    each book keeps its records while its size, mtime and the encoder are unchanged; new or changed books
    are extracted (up to SYNC_MAX_ENTITIES entities of SYNC_MAX_EVENTS events) until `budget_seconds` has
    passed (no new book starts after that; later syncs continue with the rest, reported as `pending`).
    Cross-book wildcard entries are rebuilt from the per-book ones each time.
    """
    models_dir = Path(models_dir)
    index_path = models_dir / STORY_GRAPH_NAME
    state = _load_state(models_dir)
    current = None
    if index_path.is_symlink():
        raise ReaderError(f"{STORY_GRAPH_NAME} is a symlink; not touched")
    if index_path.is_file():
        current = bounded_read(index_path, MAX_INDEX_BYTES)
        if state.get("index_sha256") != hashlib.sha256(current).hexdigest():
            return {"path": str(index_path), "prebuilt": True, "updated": False, "books": 0, "extracted": 0,
                    "pending": 0, "entities": _header(current)["num_entities"]}
    encoder, model = _encoder_identity(models_dir)
    encode = model.encode if model is not None else fallback_encode
    previous: dict[int, list[dict]] = {}
    if current is not None and state.get("encoder") == encoder:
        for entity in read_story_graph(current)["entities"]:
            if entity["book_path_hash"]:
                previous.setdefault(entity["book_path_hash"], []).append(entity)
    stamps = state.get("books", {})
    deadline = clock() + budget_seconds
    books, new_stamps, extracted, pending, unreadable = [], {}, 0, 0, 0
    for card_path, path in story_books(card_root):
        status = path.stat()
        stamp = [status.st_size, status.st_mtime_ns]
        old = stamps.get(card_path)
        book_hash = fnv1a64(card_path.encode("utf-8"))
        if isinstance(old, dict) and old.get("stamp") == stamp and (book_hash in previous or not old.get("entities")):
            books.append({"title": old.get("title", path.stem), "book_path": card_path,
                          "entities": previous.get(book_hash, []), "aliases": []})
            new_stamps[card_path] = old
            continue
        if clock() >= deadline:
            pending += 1
            continue
        try:
            book = extract_book_story_entities(path, card_path, encode=encode, max_entities=SYNC_MAX_ENTITIES,
                                               max_events=SYNC_MAX_EVENTS)
            extracted += 1
        except ReaderError:
            # Like the ICPT index: a book the reader cannot open either has no story; not retried while unchanged.
            book = {"title": path.stem, "book_path": card_path, "entities": [], "aliases": []}
            unreadable += 1
        books.append(book)
        new_stamps[card_path] = {"stamp": stamp, "title": book["title"], "entities": len(book["entities"])}
    entities = [entity for book in books for entity in book["entities"]] + cross_book_entities(books)
    result = build_story_graph_index(entities, [], index_path)
    new_state = {"version": STATE_VERSION, "encoder": encoder, "books": new_stamps, "index_sha256": result["sha256"]}
    if new_state != state:
        atomic_write(models_dir / STATE_NAME, json.dumps(new_state, indent=1, sort_keys=True,
                                                         ensure_ascii=False).encode("utf-8"))
    return {"path": str(index_path), "prebuilt": False, "updated": result["updated"], "books": len(books),
            "extracted": extracted, "unreadable": unreadable, "pending": pending, "entities": result["entities"],
            "events": result["events"]}
